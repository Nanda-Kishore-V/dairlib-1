#include <memory>
#include <chrono>
#include <random>
#include <gflags/gflags.h>


#include "drake/solvers/mosek_solver.h"
#include "drake/solvers/gurobi_solver.h"
#include "drake/solvers/mathematical_program.h"

#include "sgd_iter.h"
#include "src/manifold_constraint.h"
#include "src/file_utils.h"

using Eigen::MatrixXd;
using Eigen::VectorXd;
using std::string;

using drake::goldilocks_walking::writeCSV;
using drake::goldilocks_walking::readCSV;
using drake::solvers::VectorXDecisionVariable;

namespace drake{
namespace goldilocks_walking {

void runSGD() {
  std::random_device randgen;
  std::default_random_engine e1(randgen());
  std::uniform_real_distribution<> dist(0, 1);


  int n_batch = 5;

  // int n_weights = 43;
  int n_weights =  16;
  VectorXd theta_0 = VectorXd::Zero(n_weights);
  theta_0(0) = -0.1;
  theta_0(5) = 1.0;
  writeCSV("data/0_theta.csv", theta_0);

  double length = 0.5;
  double duration = 1;
  int snopt_iter = 200;
  string directory = "data/";
  string init_z = "z_save.csv";
  string weights = "0_theta.csv";
  string output_prefix = "0_0_";
  sgdIter(length, duration, snopt_iter, directory, init_z, weights, output_prefix);

  for (int iter = 1; iter <= 50; iter++) {
    int input_batch = iter == 1 ? 1 : n_batch;

    std::vector<MatrixXd> A_vec;
    std::vector<MatrixXd> B_vec;
    std::vector<MatrixXd> H_vec;
    std::vector<MatrixXd> A_active_vec;
    std::vector<MatrixXd> B_active_vec;
    std::vector<MatrixXd> lb_vec;
    std::vector<MatrixXd> ub_vec;
    std::vector<MatrixXd> y_vec;
    std::vector<MatrixXd> w_vec;
    std::vector<MatrixXd> z_vec;
    std::vector<MatrixXd> theta_vec;
    std::vector<double> nl_vec;
    std::vector<double> nz_vec;

    int nz,nt,nl;

    for (int batch = 0; batch < input_batch; batch++) {
      string batch_prefix = std::to_string(iter-1) + "_" + std::to_string(batch) + "_";
      string iter_prefix = std::to_string(iter-1) + "_";

      A_vec.push_back(readCSV(directory + batch_prefix + "A.csv"));
      B_vec.push_back(readCSV(directory + batch_prefix + "B.csv"));
      H_vec.push_back(readCSV(directory + batch_prefix + "H.csv"));
      lb_vec.push_back(readCSV(directory + batch_prefix + "lb.csv"));
      ub_vec.push_back(readCSV(directory + batch_prefix + "ub.csv"));
      y_vec.push_back(readCSV(directory + batch_prefix + "y.csv"));
      w_vec.push_back(readCSV(directory + batch_prefix + "w.csv"));
      z_vec.push_back(readCSV(directory + batch_prefix + "z.csv"));
      theta_vec.push_back(readCSV(directory + iter_prefix + "theta.csv"));

      DRAKE_ASSERT(w_vec[batch].cols() == 1);
      DRAKE_ASSERT(lb_vec[batch].cols() == 1);
      DRAKE_ASSERT(ub_vec[batch].cols() == 1);
      DRAKE_ASSERT(y_vec[batch].cols() == 1);
      DRAKE_ASSERT(w_vec[batch].cols() == 1);
      DRAKE_ASSERT(z_vec[batch].cols() == 1);
      DRAKE_ASSERT(theta_vec[batch].cols() == 1);


      int n_active = 0;
      double tol = 1e-4;
      for (int i = 0; i < y_vec[batch].rows(); i++) {
        if (y_vec[batch](i) >= ub_vec[batch](i) - tol || y_vec[batch](i) <= lb_vec[batch](i) + tol)
          n_active++;
      }

      int nz_i = A_vec[batch].cols();
      int nt_i = B_vec[batch].cols();

      MatrixXd A_active(n_active, nz_i);
      MatrixXd B_active(n_active, nt_i);
      MatrixXd AB_active(n_active, nz_i + nt_i);

      int nl_i = 0;
      for (int i = 0; i < y_vec[batch].rows(); i++) {
        if (y_vec[batch](i) >= ub_vec[batch](i) - tol || y_vec[batch](i) <= lb_vec[batch](i) + tol) {
          A_active.row(nl_i) = A_vec[batch].row(i);
          B_active.row(nl_i) = B_vec[batch].row(i);
          AB_active.row(nl_i) << A_vec[batch].row(i), B_vec[batch].row(i);
          nl_i++;
        }
      }

      A_active_vec.push_back(A_active);
      B_active_vec.push_back(B_active);
      nl_vec.push_back(nl_i);
      nz_vec.push_back(nz_i);

      nl += nl_i;
      nz += nz_i;
      if (batch == 0) {
        nt = nt_i;
      } else {
        DRAKE_ASSERT(nt == nt_i);
        DRAKE_ASSERT((theta_vec[0] - theta_vec[batch]).norm() == 0);
      }
    }

    //Join matricies
    MatrixXd AB_active = MatrixXd::Zero(nl,nz+nt);
    MatrixXd H_ext = MatrixXd::Zero(nz + nt, nz + nt);
    VectorXd w_ext = VectorXd::Zero(nz+nt,1);
    int nl_start = 0;
    int nz_start = 0;
    for (int batch = 0; batch < input_batch; batch++) {
      AB_active.block(nl_start, nz_start, nl_vec[batch], nz_vec[batch]) = A_active_vec[batch];
      AB_active.block(nl_start, nz, nl_vec[batch], nt) = B_active_vec[batch];

      H_ext.block(nz_start,nz_start,nz_vec[batch],nz_vec[batch]) = H_vec[batch];
      w_ext.segment(nz_start,nz_vec[batch]) = w_vec[batch].col(0);

      nl_start += nl_vec[batch];
      nz_start += nz_vec[batch];
    }
    H_ext.block(nz,nz,nt,nt) = 1e-2*MatrixXd::Identity(nt,nt);


    Eigen::BDCSVD<MatrixXd> svd(AB_active,  Eigen::ComputeFullV);

    MatrixXd N = svd.matrixV().rightCols(AB_active.cols() - svd.rank());

    auto gradient = N*N.transpose()*w_ext;

    double scale_num= gradient.dot(gradient);
    double scale_den = gradient.dot(H_ext*gradient);

    auto dtheta = -0.1*gradient.tail(nt)*scale_num/scale_den;

    std::cout << std::endl<< "dtheta norm: " << dtheta.norm() << std::endl;
    std::cout << "***********Next iteration*************" << std::endl;

    // std::cout << "scale predict: " << scale_num/scale_den << std::endl;

    writeCSV("data/" + std::to_string(iter) + "_theta.csv", theta_vec[0] + dtheta);

    // init_z = std::to_string(iter-1) + "_z.csv";
    init_z = "z_save.csv";
    weights = std::to_string(iter) +  "_theta.csv";
    output_prefix = std::to_string(iter) +  "_";

    for(int batch = 0; batch < n_batch; batch++) {
    //randomize distance on [0.3,0.5]
      length = 0.3 + 0.2*dist(e1);
      duration =  length/0.5; //maintain constaint speed of 0.5 m/s

      std::cout << std::endl << "Iter-Batch: " << iter << "-" << batch << std::endl;
      std::cout << "New length: " << length << std::endl;

      string batch_prefix = output_prefix + std::to_string(batch) + "_";

      sgdIter(length, duration, snopt_iter, directory, init_z, weights, batch_prefix);
    }
  }
}
}
}

int main(int argc, char* argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  std::srand(time(0));  // Initialize random number generator.

  drake::goldilocks_walking::runSGD();
}
