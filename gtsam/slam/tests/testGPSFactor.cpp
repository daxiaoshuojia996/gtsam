/**
 * @file    testGPSFactor.cpp
 * @brief  
 * @author Luca Carlone
 * @date   July 30, 2014
 */

#include <gtsam/base/numericalDerivative.h>
#include <gtsam/geometry/Pose3.h>
#include <gtsam/inference/Symbol.h>
#include <gtsam/slam/GPSFactor.h>
#include <CppUnitLite/TestHarness.h>

using namespace gtsam;
using namespace gtsam::symbol_shorthand;
using namespace gtsam::noiseModel;
// Convenience for named keys

using symbol_shorthand::X;
using symbol_shorthand::B;

TEST(GPSFactor, errorNoiseless) {

  Rot3 R = Rot3::rodriguez(0.1, 0.2, 0.3);
  Point3 t(1.0, 0.5, 0.2);
  Pose3 pose(R,t);
  Point3 bias(0.0,0.0,0.0);
  Point3 noise(0.0,0.0,0.0);
  Point3 measured = t + noise;

  GPSFactor factor(X(1), B(1), measured, Isotropic::Sigma(3, 0.05));
  Vector expectedError = (Vector(3) << 0.0, 0.0, 0.0 );
  Vector actualError = factor.evaluateError(pose,bias);
  EXPECT(assert_equal(expectedError,actualError, 1E-5));
}

TEST(GPSFactor, errorNoisy) {

  Rot3 R = Rot3::rodriguez(0.1, 0.2, 0.3);
  Point3 t(1.0, 0.5, 0.2);
  Pose3 pose(R,t);
  Point3 bias(0.0,0.0,0.0);
  Point3 noise(1.0,2.0,3.0);
  Point3 measured = t - noise;

  GPSFactor factor(X(1), B(1), measured, Isotropic::Sigma(3, 0.05));
  Vector expectedError = (Vector(3) << 1.0, 2.0, 3.0 );
  Vector actualError = factor.evaluateError(pose,bias);
  EXPECT(assert_equal(expectedError,actualError, 1E-5));
}

TEST(GPSFactor, jacobian) {

  Rot3 R = Rot3::rodriguez(0.1, 0.2, 0.3);
  Point3 t(1.0, 0.5, 0.2);
  Pose3 pose(R,t);
  Point3 bias(0.0,0.0,0.0);

  Point3 noise(0.0,0.0,0.0);
  Point3 measured = t + noise;

  GPSFactor factor(X(1), B(1), measured, Isotropic::Sigma(3, 0.05));

  Matrix actualH1, actualH2;
  factor.evaluateError(pose,bias, actualH1, actualH2);

  Matrix numericalH1 = numericalDerivative21(
      boost::function<Vector(const Pose3&, const Point3&)>(boost::bind(
          &GPSFactor::evaluateError, factor, _1, _2, boost::none,
          boost::none)), pose, bias, 1e-5);
  EXPECT(assert_equal(numericalH1,actualH1, 1E-5));

  Matrix numericalH2 = numericalDerivative22(
      boost::function<Vector(const Pose3&, const Point3&)>(boost::bind(
          &GPSFactor::evaluateError, factor, _1, _2, boost::none,
          boost::none)), pose, bias, 1e-5);
  EXPECT(assert_equal(numericalH2,actualH2, 1E-5));
}

/* ************************************************************************* */
int main() {
  TestResult tr;
  return TestRegistry::runAllTests(tr);
}
/* ************************************************************************* */
