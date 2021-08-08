#pragma once
#include <Eigen/Dense>
#include <iostream>
#include <string>
#include <unordered_map>

#include "drake/multibody/plant/multibody_plant.h"
#include "drake/solvers/gurobi_solver.h"
#include "drake/solvers/mathematical_program_result.h"

using ModelInstanceToVecMap =
std::unordered_map<drake::multibody::ModelInstanceIndex,
                           Eigen::VectorXd>;

struct QuasistaticSimParameters {
  Eigen::Vector3d gravity;
  size_t nd_per_contact;
  double contact_detection_tolerance;
  bool is_quasi_dynamic;
  bool requires_grad;
};

class QuasistaticSimulator {
public:
  QuasistaticSimulator(
      const std::string &model_directive_path,
      const std::unordered_map<std::string, Eigen::VectorXd>
          &robot_stiffness_str,
      const std::unordered_map<std::string, std::string> &object_sdf_paths,
      QuasistaticSimParameters sim_params);

  void UpdateMbpConfiguration(const ModelInstanceToVecMap &q_dict) const;

  [[nodiscard]] ModelInstanceToVecMap
  GetCurrentConfigurationFromContext() const;

  void Step(const ModelInstanceToVecMap &q_a_cmd_dict,
            const ModelInstanceToVecMap &tau_ext_dict,
            const double h, const double contact_detection_tolerance,
            const bool requires_grad) const;

  void Step(const ModelInstanceToVecMap &q_a_cmd_dict,
            const ModelInstanceToVecMap &tau_ext_dict,
            const double h) const;

  [[nodiscard]] const std::set<drake::multibody::ModelInstanceIndex> &
  get_models_all() const {
    return models_all_;
  };

private:
  [[nodiscard]] std::vector<int>
  GetVelocityIndicesForModel(drake::multibody::ModelInstanceIndex idx) const;

  [[nodiscard]] double GetFrictionCoefficientForSignedDistancePair(
      drake::geometry::GeometryId id_A, drake::geometry::GeometryId id_B) const;

  [[nodiscard]] drake::multibody::BodyIndex
  GetMbpBodyFromGeometry(drake::geometry::GeometryId g_id) const;

  [[nodiscard]] std::unique_ptr<drake::multibody::ModelInstanceIndex>
      FindModelForBody(drake::multibody::BodyIndex) const;

  void CalcJacobianAndPhi(const double contact_detection_tol, int *n_c_ptr,
                          int *n_f_ptr,
                          drake::EigenPtr<Eigen::VectorXd> phi_ptr,
                          drake::EigenPtr<Eigen::VectorXd> phi_constraints_ptr,
                          drake::EigenPtr<Eigen::MatrixXd> Jn_ptr,
                          drake::EigenPtr<Eigen::MatrixXd> J_ptr) const;

  void UpdateJacobianRows(const drake::multibody::BodyIndex &body_idx,
                          const Eigen::Ref<const Eigen::Vector3d> &pC_Body,
                          const Eigen::Ref<const Eigen::Vector3d> &n_W,
                          const Eigen::Ref<const Eigen::Matrix3Xd> &d_W,
                          int i_c, int n_d, int i_f_start,
                          drake::EigenPtr<Eigen::MatrixXd> Jn_ptr,
                          drake::EigenPtr<Eigen::MatrixXd> Jf_ptr) const;

  void FormQAndTauH(
      const ModelInstanceToVecMap& q_dict,
      const ModelInstanceToVecMap& q_a_cmd_dict,
      const ModelInstanceToVecMap& tau_ext_dict,
      const double h,
      drake::EigenPtr<Eigen::MatrixXd> Q_ptr,
      drake::EigenPtr<Eigen::VectorXd> tau_h_ptr) const;

  const QuasistaticSimParameters sim_params_;

  // QP solver.
  std::unique_ptr<drake::solvers::GurobiSolver> solver_;
  mutable drake::solvers::MathematicalProgramResult mp_result_;

  // Systems.
  std::unique_ptr<drake::systems::Diagram<double>> diagram_;
  drake::multibody::MultibodyPlant<double> *plant_{nullptr};
  drake::geometry::SceneGraph<double> *sg_{nullptr};

  // Contexts.
  std::unique_ptr<drake::systems::Context<double>> context_; // Diagram.
  drake::systems::Context<double> *context_plant_{nullptr};
  drake::systems::Context<double> *context_sg_{nullptr};
  mutable const drake::geometry::QueryObject<double> *query_object_{nullptr};

  // MBP introspection.
  std::set<drake::multibody::ModelInstanceIndex> models_actuated_;
  std::set<drake::multibody::ModelInstanceIndex> models_unactuated_;
  std::set<drake::multibody::ModelInstanceIndex> models_all_;
  ModelInstanceToVecMap robot_stiffness_;
  std::unordered_map<drake::multibody::ModelInstanceIndex, std::vector<int>>
      velocity_indices_;
  std::unordered_map<drake::multibody::ModelInstanceIndex,
                     std::unordered_set<drake::multibody::BodyIndex>>
      bodies_indices_;

  // friction_coefficients[g_idA][g_idB] and friction_coefficients[g_idB][g_idA]
  //  gives the coefficient of friction between contact geometries g_idA
  //  and g_idB.
  std::unordered_map<drake::geometry::GeometryId,
                     std::unordered_map<drake::geometry::GeometryId, double>>
      friction_coefficients_;
};
