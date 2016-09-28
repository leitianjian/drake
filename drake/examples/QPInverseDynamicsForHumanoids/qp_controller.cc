#include "drake/solvers/snopt_solver.h"
#include "drake/solvers/gurobi_solver.h"

#include "qp_controller.h"
#include "drake/math/cross_product.h"

using namespace drake::solvers;

void QPController::ResizeQP(const RigidBodyTree& robot,
                            const std::vector<ContactInformation>& all_supports,
                            const std::vector<DesiredBodyAcceleration>& all_body_accelerations) {
  // Figure out dimensions
  int num_contact_body = all_supports.size();
  int num_vd = robot.number_of_velocities();
  int num_basis = 0;
  int num_point_force = 0;
  for (size_t i = 0; i < all_supports.size(); i++) {
    num_point_force += all_supports[i].contact_points().size();
    num_basis += all_supports[i].num_basis();
  }
  int num_torque = robot.actuators.size();
  int num_variable = num_vd + num_basis;

  if (num_contact_body == num_contact_body_ && num_vd == num_vd_ &&
      num_basis == num_basis_ && num_point_force == num_point_force_ &&
      num_torque == num_torque_ && num_variable == num_variable_ &&
      (int)all_body_accelerations.size() == num_body_acceleration_)
    return;

  num_contact_body_ = num_contact_body;
  num_vd_ = num_vd;
  num_basis_ = num_basis;
  num_point_force_ = num_point_force;
  num_torque_ = num_torque;
  num_variable_ = num_variable;
  num_body_acceleration_ = (int)all_body_accelerations.size();

  // The order of insertion is important, the rest of the program assumes this
  // layout.
  prog_ = MathematicalProgram();
  DecisionVariableView vd = prog_.AddContinuousVariables(num_vd_, "vd");
  DecisionVariableView basis =
      prog_.AddContinuousVariables(num_basis_, "basis");

  // Allocate space for contact force jacobian and basis matrix.
  stacked_contact_jacobians_.resize(3 * num_point_force_, num_vd_);
  basis_to_force_matrix_.resize(3 * num_point_force_, num_basis_);
  stacked_contact_jacobians_dot_times_v_.resize(3 * num_point_force_);
  torque_linear_.resize(num_torque_, num_variable_);
  dynamics_linear_.resize(6, num_variable_);

  // Allocate equality constraints
  // Dyanmics
  eq_dynamics_ = prog_.AddLinearEqualityConstraint(
      MatrixXd::Zero(6, num_variable_), Matrix<double, 6, 1>::Zero(),
      {vd, basis});
  eq_dynamics_->set_description("dynamics eq");
  eq_contacts_.resize(num_contact_body_);
  // Contact constraints, 3 rows per contact point
  for (int i = 0; i < num_contact_body_; i++) {
    eq_contacts_[i] = prog_.AddLinearEqualityConstraint(
        MatrixXd::Zero(3 * all_supports[i].contact_points().size(), num_vd_),
        VectorXd::Zero(3 * all_supports[i].contact_points().size()), {vd});
    eq_contacts_[i]->set_description(all_supports[i].name() + " contact eq");
  }

  // Allocate inequality constraints
  // Contact force scalar (Beta).
  // This is constant and does not depend on the robot configuration.
  ineq_contact_wrench_ = prog_.AddLinearConstraint(
      MatrixXd::Identity(num_basis_, num_basis_), VectorXd::Zero(num_basis_),
      VectorXd::Constant(num_basis_, 1000), {basis});
  ineq_contact_wrench_->set_description("contact force basis ineq");
  // Torque limit
  ineq_torque_limit_ = prog_.AddLinearConstraint(
      MatrixXd::Zero(num_torque_, num_variable_), VectorXd::Zero(num_torque_),
      VectorXd::Zero(num_torque_), {vd, basis});
  ineq_torque_limit_->set_description("torque limit ineq");

  // Allocate cost terms
  MatrixXd tmp_matrix_vd(num_vd_, num_vd_);
  VectorXd tmp_vector_vd(num_vd_);
  // CoMdd
  cost_comdd_ = prog_.AddQuadraticCost(tmp_matrix_vd, tmp_vector_vd, {vd});
  cost_comdd_->set_description("com cost");
  cost_body_accelerations_.resize(all_body_accelerations.size());
  body_Jdv_.resize(all_body_accelerations.size());
  body_J_.resize(all_body_accelerations.size());
  for (size_t i = 0; i < all_body_accelerations.size(); i++) {
    cost_body_accelerations_[i] = prog_.AddQuadraticCost(tmp_matrix_vd, tmp_vector_vd, {vd});
    cost_body_accelerations_[i]->set_description(all_body_accelerations[i].name() + " cost");
  }
  // Regularize vd
  cost_vd_reg_ = prog_.AddQuadraticCost(tmp_matrix_vd, tmp_vector_vd, {vd});
  cost_vd_reg_->set_description("vd reg cost");
  // Regularize vd
  cost_basis_reg_ = prog_.AddQuadraticCost(MatrixXd::Identity(num_basis_, num_basis_), VectorXd::Zero(num_basis_), {basis});
  cost_basis_reg_->set_description("basis reg cost");
}

int QPController::Control(const HumanoidStatus& rs, const QPInput& input,
                          QPOutput* output) {
  if (!input.is_valid(rs.robot().number_of_velocities())) {
    std::cerr << "input is invalid\n";
    return -1;
  }

  // Resize and zero temporary matrices.
  ResizeQP(rs.robot(), input.contact_info(), input.desired_body_accelerations());
  SetTempMatricesToZero();

  ////////////////////////////////////////////////////////////////////
  // The equations of motion look like:
  // M(q) * vd + h(q,v) = S * tau + J^T * lambda
  // M(q) is the inertia matrix, h(q,v) is the gravitational and centrifugal
  // force, vd is acceleration, S is the selection matrix (top 6 rows are
  // zeros due to the floating base), tau is joint torque, J^T is the transpose
  // of all contact Jacobian, and lambda is the contact wrench in the world
  // frame.
  // In this implementation, lambda is replaced by a set of point forces
  // applied at different contact points per each contact link.
  // The equations of motion is updated to:
  // M(q) * vd + h(q,v) = S * tau + J^T * basis * Beta
  //
  // For inverse dynamics, we are usually given desired motions, and
  // we want to solve for tau to achieve those motions.
  // Desired motions can be directly specified as desired_vd, or as acceleration_d in Cartesian
  // space, which is linear w.r.t. vd as well: acceleration_d = J * vd + Jd * v.
  //
  // Note that since S.topRows(6) is zero,
  // tau = M_l * vd + h_l - (J^T * basis)_l * Beta
  // where _l means the lower num_torque_ rows of those matrices.
  // So we just need to solve for vd and Beta, and tau can be computed as
  // above. We can formulate inverse dynamics a QP problem.
  //
  // For the QP problem:
  // the unknown is _X = [vd, Beta]
  // equality constraints:
  //  M_u * vd + h_u = (J^T * basis)_u * Beta *  (equations of motion)
  //  J * vd + Jd * v = 0, (contact constraints)
  // inEquality: joint torque limit, limits on Beta, etc.
  // cost func:
  //  min (Jcom*vd + Jcomd*v - desired_comdd)^2
  //    + (vd - desired_vd)^2
  //    + all_kinds_of_body_acceleration_cost_terms (pelvis, torso, feet, etc)
  //
  // I made the dynamics and stationary contact equality constraints.
  // Alternatively, they can be setup as high weight cost terms. This is
  // sometimes preferred as it introduce slacks for better stability.

  const DecisionVariableView vd = prog_.GetVariable("vd");
  const DecisionVariableView basis = prog_.GetVariable("basis");

  int basis_start = basis.index();
  int vd_start = vd.index();

  // Stack the contact Jacobians and basis matrices for each contact link
  int rowIdx = 0;
  int colIdx = 0;
  for (int b = 0; b < num_contact_body_; b++) {
    const ContactInformation& support = input.contact_info(b);
    int force_dim = 3 * support.contact_points().size();
    int basis_dim = support.num_basis();
    basis_to_force_matrix_.block(rowIdx, colIdx, force_dim, basis_dim) =
        support.ComputeBasisMatrix(rs.robot(), rs.cache());
    stacked_contact_jacobians_.block(rowIdx, 0, force_dim, num_vd_) =
        support.ComputeJacobianAtContactPoints(rs.robot(), rs.cache());
    stacked_contact_jacobians_dot_times_v_.segment(rowIdx, force_dim) =
        support.ComputeJacobianDotTimesVAtContactPoints(rs.robot(), rs.cache());
    rowIdx += force_dim;
    colIdx += basis_dim;
  }
  JB_ = stacked_contact_jacobians_.transpose() * basis_to_force_matrix_;
  DRAKE_ASSERT(rowIdx == num_point_force_ * 3);
  DRAKE_ASSERT(colIdx == num_basis_);

  // tau = M_l * vd + h_l - (J^T * basis)_l * Beta,
  // tau = torque_linear_ * X + torque_constant_
  torque_linear_.block(0, vd_start, num_torque_, num_vd_) =
      rs.M().bottomRows(num_torque_);
  torque_linear_.block(0, basis_start, num_torque_, num_basis_) =
      -JB_.bottomRows(num_torque_);
  torque_constant_ = rs.bias_term().tail(num_torque_);

  ////////////////////////////////////////////////////////////////////
  // Equality constraints:
  // Equations of motion part, 6 rows
  dynamics_linear_.block(0, vd_start, 6, num_vd_) = rs.M().topRows(6);
  dynamics_linear_.block(0, basis_start, 6, num_basis_) = -JB_.topRows(6);
  dynamics_constant_ = -rs.bias_term().head(6);
  eq_dynamics_->UpdateConstraint(dynamics_linear_, dynamics_constant_);

  // contact constraints, 3 rows per contact point
  rowIdx = 0;
  for (int i = 0; i < num_contact_body_; i++) {
    int force_dim = 3 * input.contact_info(i).contact_points().size();
    eq_contacts_[i]->UpdateConstraint(
        stacked_contact_jacobians_.block(rowIdx, 0, force_dim, num_vd_),
        -stacked_contact_jacobians_dot_times_v_.segment(rowIdx, force_dim));
    rowIdx += force_dim;
  }
  DRAKE_ASSERT(rowIdx == num_point_force_ * 3);

  ////////////////////////////////////////////////////////////////////
  // Inequality constraints:
  // For the contact point force basis, the constraints are always > 0,
  // so the stay constant as in ResizeQP.

  // Torque limits: min <= tau <= max, num_torque_ rows
  // min <= M_l * vd + h_l - (J^T * basis)_l * Beta <= max
  // min - h_l <= M_l * vd - (J^T * basis)_l * Beta <= max - h_l
  // tau = rs.robot->B.bottomRows(num_torque_) * u,
  // u = rs.robot->B.bottomRows(num_torque_).transpose() * tau
  // since B should be orthonormal.
  // tau is joint space indexed, and u is actuator space indexed.
  // constraints are specified with u index.
  inequality_linear_ =
      rs.robot().B.bottomRows(num_torque_).transpose() * torque_linear_;
  inequality_upper_bound_ = inequality_lower_bound_ =
      -rs.robot().B.bottomRows(num_torque_).transpose() * torque_constant_;
  for (size_t i = 0; i < rs.robot().actuators.size(); i++) {
    inequality_lower_bound_[i] += rs.robot().actuators[i].effort_limit_min_;
    inequality_upper_bound_[i] += rs.robot().actuators[i].effort_limit_max_;
  }
  ineq_torque_limit_->UpdateConstraint(
      inequality_linear_, inequality_lower_bound_, inequality_upper_bound_);

  ////////////////////////////////////////////////////////////////////
  // Cost function:
  // CoM term (task space acceleration costs)
  // w * (J * vd + Jdv - desired_comdd)^T * (J * vd + Jdv - desired_comdd)
  cost_comdd_->UpdateConstraint(
      input.w_com() * rs.J_com().transpose() * rs.J_com(),
      input.w_com() * rs.J_com().transpose() *
          (rs.Jdot_times_v_com() - input.desired_comdd()));

  const std::vector<DesiredBodyAcceleration>& body_motions_d = input.desired_body_accelerations();
  for (size_t i = 0; i < body_motions_d.size(); i++) {
    const DesiredBodyAcceleration& body_motion_d = body_motions_d[i];
    body_J_[i] = GetTaskSpaceJacobian(rs.robot(), rs.cache(), body_motion_d.body(), Vector3d::Zero());
    body_Jdv_[i] = GetTaskSpaceJacobianDotTimesV(rs.robot(), rs.cache(), body_motion_d.body(), Vector3d::Zero());

    cost_body_accelerations_[i]->UpdateConstraint(
        body_motion_d.weight() * body_J_[i].transpose() * body_J_[i],
        body_motion_d.weight() * body_J_[i].transpose() *
            (body_Jdv_[i] - body_motion_d.acceleration()));
  }
  // Regularize vd to desired_vd
  cost_vd_reg_->UpdateConstraint(
      input.w_vd() * MatrixXd::Identity(num_vd_, num_vd_),
      input.w_vd() * (-input.desired_vd()));
  // Regularize basis to zero
  cost_basis_reg_->UpdateConstraint(
      input.w_basis_reg() * MatrixXd::Identity(num_basis_, num_basis_),
      VectorXd::Zero(num_basis_));

  ////////////////////////////////////////////////////////////////////
  // Call solver
  SolutionResult result;
  SnoptSolver solver_;
  if (!solver_.available()) {
    std::cerr << "Solver not available.\n";
    return -1;
  }
  result = solver_.Solve(prog_);
  if (result != drake::solvers::SolutionResult::kSolutionFound) {
    std::cerr << "solution not found\n";
    return -1;
  }
  VectorXd solution = prog_.GetSolution();

  ////////////////////////////////////////////////////////////////////
  // Example of inspecting each cost / eq, ineq term
  auto costs = prog_.quadratic_costs();
  auto eqs = prog_.linear_equality_constraints();
  auto ineqs = prog_.linear_constraints();

  output->mutable_costs().resize(costs.size());
  int ctr = 0;
  for (auto cost_b : costs) {
    VectorXd val;
    std::shared_ptr<Constraint> cost = cost_b.constraint();
    cost->Eval(cost_b.VariableListToVectorXd(), val);
    output->mutable_cost(ctr).first = cost->get_description();
    output->mutable_cost(ctr).second = val(0);
    ctr++;
  }

  for (auto eq_b : eqs) {
    std::shared_ptr<LinearEqualityConstraint> eq = eq_b.constraint();
    VectorXd X = eq_b.VariableListToVectorXd();
    DRAKE_ASSERT((eq->A() * X - eq->lower_bound()).isZero(EPSILON));
  }

  for (auto ineq_b : ineqs) {
    std::shared_ptr<LinearConstraint> ineq = ineq_b.constraint();
    VectorXd X = ineq_b.VariableListToVectorXd();
    X = ineq->A() * X;
    for (int i = 0; i < X.size(); i++) {
      DRAKE_ASSERT(X[i] >= ineq->lower_bound()[i] - EPSILON &&
                   X[i] <= ineq->upper_bound()[i] + EPSILON);
    }
  }

  ////////////////////////////////////////////////////////////////////
  // Parse result
  // Compute resulting contact wrenches
  int basis_index = 0;
  int point_force_index = 0;
  point_forces_ = basis_to_force_matrix_ * basis.value();

  output->mutable_resolved_contacts().clear();
  for (size_t i = 0; i < input.contact_info().size(); i++) {
    const ContactInformation &contact_info = input.contact_info(i);
    output->mutable_resolved_contacts().push_back(ResolvedContact(contact_info.body()));
    ResolvedContact &resolved_contact = output->mutable_resolved_contacts().back();
    // Copy basis
    resolved_contact.mutable_basis() = basis.value().segment(basis_index, contact_info.num_basis());
    basis_index += contact_info.num_basis();

    // Compute contact points and reference point in the world frame.
    contact_info.ComputeContactPointsAndWrenchReferencePoint(rs.robot(), rs.cache(), Vector3d::Zero(), &resolved_contact.mutable_contact_points(), &resolved_contact.mutable_reference_point());

    // Convert point forces to an equivalent wrench wrt to the reference point in the world frame.
    resolved_contact.mutable_equivalent_wrench() = contact_info.ComputeWrenchMatrix(resolved_contact.contact_points(), resolved_contact.reference_point()) * point_forces_.segment(point_force_index, 3 * contact_info.num_contact_points());

    // Copy point forces
    resolved_contact.mutable_point_forces().resize(contact_info.num_contact_points());
    for (int j = 0; j < contact_info.num_contact_points(); j++) {
      resolved_contact.mutable_point_force(j) = point_forces_.segment<3>(point_force_index);
      point_force_index += 3;
    }
  }

  // Set output accelerations
  output->mutable_vd() = vd.value();
  output->mutable_comdd() = rs.J_com() * output->vd() + rs.Jdot_times_v_com();

  std::vector<BodyAcceleration>& body_accelerations = output->mutable_body_accelerations();
  body_accelerations.clear();
  for (size_t i = 0; i < input.desired_body_accelerations().size(); i++) {
    body_accelerations.push_back(BodyAcceleration(input.desired_body_acceleration(i).body()));
    body_accelerations.back().mutable_acceleration() = body_J_[i] * output->vd() + body_Jdv_[i];
  }

  // Set output joint torques
  output->mutable_joint_torque() = torque_linear_ * solution + torque_constant_;

  ////////////////////////////////////////////////////////////////////
  // Sanity check, net external wrench should = centroidal_matrix * vd +
  // centroidal_matrix_dot * v
  Vector6d Ld = rs.centroidal_momentum_matrix() * output->vd() +
                rs.centroidal_momentum_matrix_dot_times_v();
  Vector6d net_wrench = rs.robot().getMass() * rs.robot().a_grav;
  for (int i = 0; i < num_contact_body_; i++) {
    const Vector6d& contact_wrench = output->resolved_contact(i).equivalent_wrench();
    const Vector3d& ref_point = output->resolved_contact(i).reference_point();
    net_wrench += contact_wrench;
    net_wrench.segment<3>(0) +=
        (ref_point - rs.com()).cross(contact_wrench.segment<3>(3));
  }
  DRAKE_ASSERT((net_wrench - Ld).isZero(EPSILON));

  if (!output->is_valid(rs.robot().number_of_velocities(), rs.robot().actuators.size())) {
    std::cerr << "output is invalid\n";
    return -1;
  }

  return 0;
}

std::ostream& operator<<(std::ostream& out, const QPInput& input) {
  out << "===============================================\n";
  out << "QPInput:\n";
  out << "desired_comdd: " << input.desired_comdd().transpose() << std::endl;
  for (size_t i = 0; i < input.desired_body_accelerations().size(); i++) {
    const DesiredBodyAcceleration &body_motion_d = input.desired_body_acceleration(i);
    out << body_motion_d.name() << "_d" << body_motion_d.acceleration().transpose() << std::endl;
  }
  out << "desired_vd: " << input.desired_vd().transpose() << std::endl;

  out << "w_com: " << input.w_com() << std::endl;
  for (size_t i = 0; i < input.desired_body_accelerations().size(); i++) {
    const DesiredBodyAcceleration &body_motion_d = input.desired_body_acceleration(i);
    out << "w_" << body_motion_d.name() << ": " << body_motion_d.weight() << std::endl;
  }
  out << "w_vd: " << input.w_vd() << std::endl;
  out << "w_basis_reg: " << input.w_basis_reg() << std::endl;
  return out;
}

std::ostream& operator<<(std::ostream& out, const QPOutput& output) {
  out << "===============================================\n";
  out << "QPOutput:\n";
  out << "accelerations:\n";
  for (int i = 0; i < output.vd().size(); i++)
    out << output.coord_name(i) << ": " << output.vd()[i] << std::endl;

  out << "com acc: ";
  out << output.comdd().transpose() << std::endl;

  for (size_t i = 0; i < output.body_accelerations().size(); i++) {
    const BodyAcceleration& body_motion = output.body_acceleration(i);
    out << body_motion.name() << " acc: " << body_motion.acceleration().transpose() << std::endl;
  }

  out << "===============================================\n";
  for (size_t i = 0; i < output.resolved_contacts().size(); i++) {
    const ResolvedContact& contact_result = output.resolved_contact(i);
    out << contact_result.name() << " wrench: " << contact_result.equivalent_wrench().transpose() << std::endl;
    out << "point forces:\n";
    for (size_t j = 0; j < contact_result.point_forces().size(); j++) {
      out << contact_result.point_force(j).transpose() << std::endl;
    }
  }

  out << "===============================================\n";
  out << "torque:\n";
  for (int i = 0; i < output.joint_torque().size(); i++)
    out << output.coord_name(i + 6) << ": " << output.joint_torque()[i]
        << std::endl;
  out << "===============================================\n";
  out << "costs:\n";
  for (size_t i = 0; i < output.costs().size(); i++)
    out << output.costs(i).first << ": " << output.costs(i).second << std::endl;

  return out;
}
