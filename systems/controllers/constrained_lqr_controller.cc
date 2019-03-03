#include "systems/controllers/constrained_lqr_controller.h"

namespace dairlib {
namespace systems {

using std::make_unique;
using std::unique_ptr;
using Eigen::VectorXd;
using Eigen::MatrixXd;
using Eigen::HouseholderQR;
using dairlib::multibody::ContactInfo;
using dairlib::multibody::ContactToolkit;
using drake::AutoDiffXd;
using drake::AutoDiffVecXd;
using drake::math::initializeAutoDiff;
using drake::math::autoDiffToGradientMatrix;
using drake::math::autoDiffToValueMatrix;
using drake::systems::Context;
using drake::systems::controllers::LinearQuadraticRegulator;
using drake::systems::controllers::LinearQuadraticRegulatorResult;

ConstrainedLQRController::ConstrainedLQRController(
    const RigidBodyTree<double>& tree, ContactInfo contact_info)
    : AffineController(tree.get_num_positions(), tree.get_num_velocities(),
                       tree.get_num_actuators()),
      tree_(tree),
      contact_info_(contact_info),
      contact_toolkit_(
          make_unique<ContactToolkit<AutoDiffXd>>(tree, contact_info)),
      num_positions_(tree.get_num_positions()),
      num_velocities_(tree.get_num_velocities()),
      num_states_(num_positions_ + num_velocities_),
      num_efforts_(tree.get_num_actuators()),
      num_forces_(tree.getNumPositionConstraints() +
                  contact_info.num_contacts * 3) {
  // Output port that outputs the constant AffineParams computed by
  // SetupController
  output_port_params_index_ =
      this->DeclareVectorOutputPort(AffineParams(num_states_, num_efforts_),
                                    &ConstrainedLQRController::CalcControl)
          .get_index();
}

void ConstrainedLQRController::SetupController(VectorXd q0, VectorXd u0,
                                               VectorXd lambda0, MatrixXd Q,
                                               MatrixXd R) {
  // checking the validity of the parameters
  DRAKE_DEMAND(q0.size() == num_positions_);
  DRAKE_DEMAND(u0.size() == num_efforts_);
  DRAKE_DEMAND(lambda0.size() == num_forces_);

  // Creating the full state vector (Velocities are zero as it is a fixed point)
  VectorXd x0(num_states_);
  x0 << q0, VectorXd::Zero(num_velocities_);
  VectorX<AutoDiffXd> x0_autodiff = initializeAutoDiff(x0);

  // Computing the full Jacobian (Position and contact)
  MatrixXd J_tree, J_contact;
  KinematicsCache<double> k_cache = tree_.doKinematics(q0);

  // Position Jacobian
  J_tree = tree_.positionConstraintsJacobian(k_cache);

  // Contact Jacobian (If contact information is provided)
  if (contact_info_.num_contacts > 0) {
    J_contact = autoDiffToValueMatrix(
        contact_toolkit_->CalcContactJacobian(x0_autodiff));
  }

  MatrixXd J(J_tree.rows() + J_contact.rows(), num_positions_);
  J << J_tree, J_contact;

  // Computing F
  MatrixXd F = MatrixXd::Zero(2 * J.rows(), 2 * J.cols());
  F.block(0, 0, J.rows(), J.cols()) = J;
  F.block(J.rows(), J.cols(), J.rows(), J.cols()) = J;

  // Computing the null space of F
  HouseholderQR<MatrixXd> qr_decomp(F.transpose());
  MatrixXd q_decomp = qr_decomp.householderQ();

  MatrixXd P =
      q_decomp.block(0, F.rows(), q_decomp.rows(), q_decomp.cols() - F.rows());
  P.transposeInPlace();

  // To compute the linearization, xdot (autodiff) is computed
  // Creating a combined autodiff vector and then extracting the individual
  // components to ensure proper gradient initialization.
  VectorXd xul0(num_states_ + num_efforts_ + num_forces_);
  xul0 << x0, u0, lambda0;
  AutoDiffVecXd xul0_autodiff = initializeAutoDiff(xul0);
  x0_autodiff = xul0_autodiff.head(num_states_);
  AutoDiffVecXd u0_autodiff = xul0.segment(num_states_, num_efforts_);
  AutoDiffVecXd lambda0_autodiff = xul0.tail(num_forces_);

  // xdot
  AutoDiffVecXd xdot0_autodiff = contact_toolkit_->CalcTimeDerivatives(
      x0_autodiff, u0_autodiff, lambda0_autodiff);

  // Making sure that the derivative is zero
  DRAKE_DEMAND(autoDiffToValueMatrix(xdot0_autodiff).isZero(1e-6));

  MatrixXd AB = autoDiffToGradientMatrix(xdot0_autodiff);
  MatrixXd A = AB.leftCols(num_states_);
  MatrixXd B = AB.block(0, num_states_, AB.rows(), num_efforts_);

  // A and B matrices in the new coordinates
  A_ = P * A * P.transpose();
  B_ = P * B;

  // Validating the dimesions of A_, B_, Q and R with respect to each other.
  DRAKE_DEMAND(Q.rows() == Q.cols());
  DRAKE_DEMAND(R.rows() == R.cols());
  DRAKE_DEMAND(A_.rows() == Q.rows());
  DRAKE_DEMAND(B_.cols() == R.rows());

  lqr_result_ = LinearQuadraticRegulator(A_, B_, Q, R);
  K_ = lqr_result_.K * P;
  E_ = u0;
  x_desired_ = x0;
}

void ConstrainedLQRController::CalcControl(const Context<double>& context,
                                           AffineParams* control) const {
  control->set_K(K_);
  control->set_E(E_);
  control->set_desired_state(x_desired_);
  control->set_timestamp(context.get_time());
}

}  // namespace systems
}  // namespace dairlib
