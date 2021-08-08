#include <set>
#include <vector>

#include "drake/multibody/parsing/parser.h"
#include "drake/multibody/parsing/process_model_directives.h"
#include "drake/systems/framework/diagram_builder.h"
#include "drake/solvers/mathematical_program.h"

#include "quasistatic_simulator.h"

using drake::multibody::ModelInstanceIndex;
using Eigen::MatrixXd;
using Eigen::Vector3d;
using Eigen::VectorXd;
using std::string;
using std::vector;

void CreateMbp(
    drake::systems::DiagramBuilder<double> *builder,
    const string &model_directive_path,
    const std::unordered_map<string, VectorXd> &robot_stiffness_str,
    const std::unordered_map<string, string> &object_sdf_paths,
    const Eigen::Ref<const Vector3d> &gravity,
    drake::multibody::MultibodyPlant<double> **plant,
    drake::geometry::SceneGraph<double> **scene_graph,
    std::set<ModelInstanceIndex> *robot_models,
    std::set<ModelInstanceIndex> *object_models,
    std::unordered_map<ModelInstanceIndex, Eigen::VectorXd> *robot_stiffness) {
  std::tie(*plant, *scene_graph) =
      drake::multibody::AddMultibodyPlantSceneGraph(builder, 1e-3);
  auto parser = drake::multibody::Parser(*plant, *scene_graph);
  // TODO: add package path.
  drake::multibody::parsing::ProcessModelDirectives(
      drake::multibody::parsing::LoadModelDirectives(model_directive_path),
      *plant, nullptr, &parser);

  // Objects.
  for (const auto &[name, sdf_path] : object_sdf_paths) {
    object_models->insert(parser.AddModelFromFile(sdf_path, name));
  }

  // Robots.
  for (const auto &[name, Kp] : robot_stiffness_str) {
    auto robot_model = (*plant)->GetModelInstanceByName(name);
    robot_models->insert(robot_model);
    robot_stiffness->at(robot_model) = Kp;
  }

  // Gravity.
  (*plant)->mutable_gravity_field().set_gravity_vector(gravity);
  (*plant)->Finalize();
}

Eigen::Matrix3Xd CalcTangentVectors(const Vector3d &normal, const size_t nd) {
  Eigen::Vector3d n = normal.normalized();
  Eigen::Matrix3Xd tangents(3, 2);
  if (nd == 2) {
    // Makes sure that dC is in the yz plane.
    Eigen::Vector3d n_x(1, 0, 0);
    tangents.col(0) = n_x.cross(n);
    tangents.col(1) = -tangents.col(0);
  } else {
    throw std::runtime_error("not implemented yet!");
  }

  return tangents;
}

QuasistaticSimulator::QuasistaticSimulator(
    const std::string &model_directive_path,
    const std::unordered_map<std::string, Eigen::VectorXd> &robot_stiffness_str,
    const std::unordered_map<std::string, std::string> &object_sdf_paths,
    QuasistaticSimParameters sim_params)
    : sim_params_(std::move(sim_params)),
      solver_(std::make_unique<drake::solvers::GurobiSolver>()) {
  auto builder = drake::systems::DiagramBuilder<double>();

  CreateMbp(&builder, model_directive_path, robot_stiffness_str,
            object_sdf_paths, sim_params_.gravity, &plant_, &sg_,
            &models_actuated_, &models_unactuated_, &robot_stiffness_);
  // All models instances.
  models_all_ = models_unactuated_;
  models_all_.insert(models_actuated_.begin(), models_actuated_.end());
  diagram_ = builder.Build();

  // TODO: 2D models only.
  DRAKE_THROW_UNLESS(plant_->num_velocities() == plant_->num_positions());

  // Contexts.
  context_ = diagram_->CreateDefaultContext();
  context_plant_ =
      &(diagram_->GetMutableSubsystemContext(*plant_, context_.get()));
  context_sg_ = &(diagram_->GetMutableSubsystemContext(*sg_, context_.get()));

  // MBP introspection.
  for (const auto &model : models_all_) {
    velocity_indices_[model] = GetVelocityIndicesForModel(model);
    const auto body_indices = plant_->GetBodyIndices(model);
    bodies_indices_[model].insert(body_indices.begin(), body_indices.end());
  }

  // friction coefficients.
  const auto &inspector = sg_->model_inspector();
  const auto cc = inspector.GetCollisionCandidates();
  for (const auto &[g_idA, g_idB] : cc) {
    const double mu = GetFrictionCoefficientForSignedDistancePair(g_idA, g_idB);
    friction_coefficients_[g_idA][g_idB] = mu;
    friction_coefficients_[g_idB][g_idA] = mu;
  }
}

std::vector<int> QuasistaticSimulator::GetVelocityIndicesForModel(
    drake::multibody::ModelInstanceIndex idx) const {
  std::vector<double> selector(plant_->num_velocities());
  std::iota(selector.begin(), selector.end(), 0);
  Eigen::Map<VectorXd> selector_eigen(selector.data(), selector.size());

  const auto indices_d = plant_->GetVelocitiesFromArray(idx, selector_eigen);
  std::vector<int> indices(indices_d.size());
  for (size_t i = 0; i < indices_d.size(); i++) {
    indices[i] = roundl(indices_d[i]);
  }
  return indices;
}

double QuasistaticSimulator::GetFrictionCoefficientForSignedDistancePair(
    drake::geometry::GeometryId id_A, drake::geometry::GeometryId id_B) const {
  const auto &inspector = sg_->model_inspector();
  const auto props_A = inspector.GetProximityProperties(id_A);
  const auto props_B = inspector.GetProximityProperties(id_B);
  const auto &geometryA_friction =
      props_A->GetProperty<drake::multibody::CoulombFriction<double>>(
          "material", "coulomb_friction");
  const auto &geometryB_friction =
      props_B->GetProperty<drake::multibody::CoulombFriction<double>>(
          "material", "coulomb_friction");
  auto cf = drake::multibody::CalcContactFrictionFromSurfaceProperties(
      geometryA_friction, geometryB_friction);
  return cf.static_friction();
}

drake::multibody::BodyIndex QuasistaticSimulator::GetMbpBodyFromGeometry(
    drake::geometry::GeometryId g_id) const {
  const auto &inspector = sg_->model_inspector();
  return plant_->GetBodyFromFrameId(inspector.GetFrameId(g_id))->index();
}


/*
 * Similar to the python implementation, this function updates context_plant_
 * and query_object_.
 */
void QuasistaticSimulator::UpdateMbpConfiguration(
    const ModelInstanceToVecMap &q_dict) const {
  for (const auto &model : models_all_) {
    plant_->SetPositions(context_plant_, model, q_dict.at(model));
  }

  query_object_ = &(
      sg_->get_query_output_port().Eval<drake::geometry::QueryObject<double>>(
          *context_sg_));
}

std::unordered_map<drake::multibody::ModelInstanceIndex, Eigen::VectorXd>
QuasistaticSimulator::GetCurrentConfigurationFromContext() const {
  std::unordered_map<drake::multibody::ModelInstanceIndex, Eigen::VectorXd>
      q_dict;
  for (const auto &model : models_all_) {
    q_dict[model] = plant_->GetPositions(*context_plant_, model);
  }
  return q_dict;
}

/*
 * Returns nullptr if body_idx is not in any of the values of bodies_indices_;
 * Otherwise returns the model instance to which body_idx belongs.
 */
std::unique_ptr<ModelInstanceIndex> QuasistaticSimulator::FindModelForBody(
    drake::multibody::BodyIndex body_idx) const {
  for (const auto &[model, body_indices] : bodies_indices_) {
    auto search = body_indices.find(body_idx);
    if (search != body_indices.end()) {
      return std::make_unique<ModelInstanceIndex>(model);
    }
  }
  return nullptr;
}

void QuasistaticSimulator::UpdateJacobianRows(
    const drake::multibody::BodyIndex &body_idx,
    const Eigen::Ref<const Eigen::Vector3d> &pC_Body,
    const Eigen::Ref<const Eigen::Vector3d> &n_W,
    const Eigen::Ref<const Eigen::Matrix3Xd> &d_W, int i_c, int n_d,
    int i_f_start, drake::EigenPtr<Eigen::MatrixXd> Jn_ptr,
    drake::EigenPtr<Eigen::MatrixXd> Jf_ptr) const {
  Eigen::Matrix3Xd Ji(3, plant_->num_velocities());
  const auto &frameB = plant_->get_body(body_idx).body_frame();
  plant_->CalcJacobianTranslationalVelocity(
      *context_plant_, drake::multibody::JacobianWrtVariable::kV, frameB,
      pC_Body, plant_->world_frame(), plant_->world_frame(), &Ji);

  (*Jn_ptr).row(i_c) += n_W.transpose() * Ji;
  for (int i = 0; i < n_d; i++) {
    (*Jf_ptr).row(i + i_f_start) += d_W.col(i).transpose() * Ji;
  }
}

void QuasistaticSimulator::CalcJacobianAndPhi(
    const double contact_detection_tol, int *n_c_ptr, int *n_f_ptr,
    drake::EigenPtr<VectorXd> phi_ptr,
    drake::EigenPtr<VectorXd> phi_constraints_ptr,
    drake::EigenPtr<MatrixXd> Jn_ptr, drake::EigenPtr<MatrixXd> J_ptr) const {
  // Collision queries.
  const auto &sdps = query_object_->ComputeSignedDistancePairwiseClosestPoints(
      contact_detection_tol);

  // Contact Jacobians.
  auto &n_c = *n_c_ptr;
  n_c = sdps.size();
  const int n_v = plant_->num_velocities();
  const int n_d = sim_params_.nd_per_contact;
  auto &n_f = *n_f_ptr;
  n_f = n_d * n_c;

  Eigen::Ref<VectorXd>& phi = *phi_ptr;
  phi.resize(n_c);
  Eigen::Ref<MatrixXd> &Jn = *Jn_ptr;
  Jn.resize(n_c, n_v);
  Jn.setZero();

  VectorXd U(n_c);
  MatrixXd Jf(n_f, n_v);
  Jf.setZero();
  const auto &inspector = sg_->model_inspector();

  int i_f_start = 0;
  for (int i_c = 0; i_c < n_c; i_c++) {
    const auto &sdp = sdps[i_c];
    phi[i_c] = sdp.distance;
    U[i_c] = friction_coefficients_.at(sdp.id_A).at(sdp.id_B);
    const auto bodyA_idx = GetMbpBodyFromGeometry(sdp.id_A);
    const auto bodyB_idx = GetMbpBodyFromGeometry(sdp.id_B);
    const auto &X_AGa = inspector.GetPoseInFrame(sdp.id_A);
    const auto &X_AGb = inspector.GetPoseInFrame(sdp.id_B);
    const auto p_ACa_A = X_AGa * sdp.p_ACa;
    const auto p_BCb_B = X_AGb * sdp.p_BCb;

    // TODO: it is assumed contact exists only between model
    //  instances, not between bodies within the same model instance.
    const auto model_A_ptr = FindModelForBody(bodyA_idx);
    const auto model_B_ptr = FindModelForBody(bodyB_idx);

    if (model_A_ptr and model_B_ptr) {
      const auto n_A_W = sdp.nhat_BA_W;
      const auto d_A_W = CalcTangentVectors(n_A_W, n_d);
      const auto n_B_W = -n_A_W;
      const auto d_B_W = -d_A_W;

      UpdateJacobianRows(bodyA_idx, p_ACa_A, n_A_W, d_A_W, i_c, n_d, i_f_start,
                         &Jn, &Jf);
      UpdateJacobianRows(bodyB_idx, p_BCb_B, n_B_W, d_B_W, i_c, n_d, i_f_start,
                         &Jn, &Jf);
    } else if (model_A_ptr) {
      const auto n_A_W = sdp.nhat_BA_W;
      const auto d_A_W = CalcTangentVectors(n_A_W, n_d);
      UpdateJacobianRows(bodyA_idx, p_ACa_A, n_A_W, d_A_W, i_c, n_d, i_f_start,
                         &Jn, &Jf);
    } else if (model_B_ptr) {
      const auto n_B_W = -sdp.nhat_BA_W;
      const auto d_B_W = CalcTangentVectors(n_B_W, n_d);
      UpdateJacobianRows(bodyB_idx, p_BCb_B, n_B_W, d_B_W, i_c, n_d, i_f_start,
                         &Jn, &Jf);
    } else {
      throw std::runtime_error(
          "One body in a contact pair is not in body_indices_");
    }
    i_f_start += n_d;
  }

  // Jacobian for constraints.
  Eigen::Ref<VectorXd> &phi_constraints = *phi_constraints_ptr;
  phi_constraints.resize(n_f);
  Eigen::Ref<MatrixXd> &J = *J_ptr;
  J = Jf;

  int j_start = 0;
  for (int i_c = 0; i_c < n_c; i_c++) {
    for (int j = 0; j < n_d; j++) {
      int idx = j_start + j;
      J.row(idx) = Jn.row(i_c) + U[i_c] * Jf.row(idx);
      phi_constraints[idx] = phi[i_c];
    }
    j_start += n_d;
  }
}

void QuasistaticSimulator::FormQAndTauH(
    const ModelInstanceToVecMap& q_dict,
    const ModelInstanceToVecMap& q_a_cmd_dict,
    const ModelInstanceToVecMap& tau_ext_dict,
    const double h,
    drake::EigenPtr<Eigen::MatrixXd> Q_ptr,
    drake::EigenPtr<Eigen::VectorXd> tau_h_ptr) const {
  const int n_v = plant_->num_velocities();
  MatrixXd M;
  plant_->CalcMassMatrix(*context_plant_, &M);

  Eigen::Ref<MatrixXd>& Q = *Q_ptr;
  Q = MatrixXd::Zero(n_v, n_v);
  Eigen::Ref<VectorXd>& tau_h = *tau_h_ptr;
  tau_h = VectorXd::Zero(n_v);

  for (const auto& model : models_unactuated_) {
    const auto& idx_v = velocity_indices_.at(model);
    const VectorXd& tau_ext = tau_ext_dict.at(model);

    for (const auto i: idx_v) {
      tau_h(i) = tau_ext(i);
    }

    if (sim_params_.is_quasi_dynamic) {
      for (const auto i : idx_v) {
        for (const auto j : idx_v) {
          Q(i, j) = M(i, j);
        }
      }
    }
  }

  for (const auto& model : models_actuated_) {
    const auto& idx_v = velocity_indices_.at(model);
    VectorXd dq_a_cmd = q_a_cmd_dict.at(model) - q_dict.at(model);
    const auto& Kp = robot_stiffness_.at(model);
    VectorXd tau_a_h = Kp.array() * dq_a_cmd.array();
    tau_a_h += tau_ext_dict.at(model);
    tau_a_h *= h;

    for (const auto i: idx_v) {
      tau_h(i) = tau_a_h(i);
    }

    for (int i = 0; i < idx_v.size(); i++) {
      int idx = idx_v[i];
      Q(idx, idx) = Kp(i) * h * h;
    }
  }
}

/*
 1. Extracts q_dict, a dictionary containing current system
    configuration, from context_plant_.
 2. Runs collision query and computes contact Jacobians by calling
    CalcJacobianAndPhi.
 3. Constructs and solves the quasistatic QP described in the paper.
 4. Integrates q_dict to the next time step.
 5. Calls UpdateMbpConfiguration with the new q_dict.
 */
void QuasistaticSimulator::Step(
    const ModelInstanceToVecMap &q_a_cmd_dict,
    const ModelInstanceToVecMap &tau_ext_dict,
    const double h, const double contact_detection_tolerance,
    const bool requires_grad) const {
  auto q_dict = GetCurrentConfigurationFromContext();
  const int n_v = plant_->num_velocities();

  // Compute contact jacobians.
  int n_c, n_f;
  VectorXd phi, phi_constraints;
  MatrixXd Jn, J;
  CalcJacobianAndPhi(contact_detection_tolerance, &n_c, &n_f, &phi,
                     &phi_constraints, &Jn, &J);

  // form Q and tau_h
  MatrixXd Q;
  VectorXd tau_h;
  FormQAndTauH(q_dict, q_a_cmd_dict, tau_ext_dict, h, &Q, &tau_h);

  // construct and solve MathematicalProgram.
  drake::solvers::MathematicalProgram prog;
  auto v = prog.NewContinuousVariables(n_v, "v");
  prog.AddQuadraticCost(Q, -tau_h, v);

  const VectorXd e = phi_constraints / h;
  auto constraints = prog.AddLinearConstraint(
      -J, VectorXd::Constant(n_f, -std::numeric_limits<double>::infinity()),
                             e, v);

  solver_->Solve(prog, {}, {}, &mp_result_);
  DRAKE_THROW_UNLESS(mp_result_.is_success());

  const VectorXd v_star = mp_result_.GetSolution(v);
  const VectorXd beta_star = -mp_result_.GetDualSolution(constraints);

  // Update q_dict.
  for (const auto& model : models_all_) {
    const auto& idx_v = velocity_indices_.at(model);
    auto& q_model = q_dict[model];

    for (int i = 0; i < idx_v.size(); i++) {
      int idx = idx_v[i];
      q_model[i] += v_star[idx] * h;
    }
  }

  // Update context_plant_ using the new q_dict.
  UpdateMbpConfiguration(q_dict);
}

void QuasistaticSimulator::Step(
    const ModelInstanceToVecMap &q_a_cmd_dict,
    const ModelInstanceToVecMap &tau_ext_dict,
    const double h) const {
  Step(q_a_cmd_dict, tau_ext_dict, h,
       sim_params_.contact_detection_tolerance,
       sim_params_.requires_grad);
}
