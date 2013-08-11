/* ----------------------------------------------------------------------------

* GTSAM Copyright 2010, Georgia Tech Research Corporation,
* Atlanta, Georgia 30332-0415
* All Rights Reserved
* Authors: Frank Dellaert, et al. (see THANKS for the full author list)

* See LICENSE for the license information
* -------------------------------------------------------------------------- */

/**
* @file    timeIncremental.cpp
* @brief   Incremental and batch solving, timing, and accuracy comparisons
* @author  Richard Roberts
*/

#include <gtsam/base/timing.h>
#include <gtsam/slam/dataset.h>
#include <gtsam/geometry/Pose2.h>
#include <gtsam/slam/PriorFactor.h>
#include <gtsam/slam/BetweenFactor.h>
#include <gtsam/slam/BearingRangeFactor.h>
#include <gtsam/nonlinear/Symbol.h>
#include <gtsam/nonlinear/ISAM2.h>
#include <gtsam/nonlinear/GaussNewtonOptimizer.h>
#include <gtsam/nonlinear/Marginals.h>

#include <fstream>
#include <boost/archive/binary_oarchive.hpp>
#include <boost/archive/binary_iarchive.hpp>
#include <boost/serialization/export.hpp>
#include <boost/program_options.hpp>
#include <boost/range/algorithm/set_algorithm.hpp>
#include <boost/random.hpp>

using namespace std;
using namespace gtsam;
using namespace gtsam::symbol_shorthand;
namespace po = boost::program_options;
namespace br = boost::range;

typedef Pose2 Pose;

typedef NoiseModelFactor1<Pose> NM1;
typedef NoiseModelFactor2<Pose,Pose> NM2;
typedef BearingRangeFactor<Pose,Point2> BR;

BOOST_CLASS_EXPORT(Value);
BOOST_CLASS_EXPORT(Pose);
BOOST_CLASS_EXPORT(Rot2);
BOOST_CLASS_EXPORT(Point2);
BOOST_CLASS_EXPORT(NonlinearFactor);
BOOST_CLASS_EXPORT(NoiseModelFactor);
BOOST_CLASS_EXPORT(NM1);
BOOST_CLASS_EXPORT(NM2);
BOOST_CLASS_EXPORT(BetweenFactor<Pose>);
BOOST_CLASS_EXPORT(PriorFactor<Pose>);
BOOST_CLASS_EXPORT(BR);
BOOST_CLASS_EXPORT(noiseModel::Base);
BOOST_CLASS_EXPORT(noiseModel::Isotropic);
BOOST_CLASS_EXPORT(noiseModel::Gaussian);
BOOST_CLASS_EXPORT(noiseModel::Diagonal);
BOOST_CLASS_EXPORT(noiseModel::Unit);

double chi2_red(const gtsam::NonlinearFactorGraph& graph, const gtsam::Values& config) {
  // Compute degrees of freedom (observations - variables)
  // In ocaml, +1 was added to the observations to account for the prior, but
  // the factor graph already includes a factor for the prior/equality constraint.
  //  double dof = graph.size() - config.size();
  int graph_dim = 0;
  BOOST_FOREACH(const boost::shared_ptr<gtsam::NonlinearFactor>& nlf, graph) {
    graph_dim += (int)nlf->dim();
  }
  double dof = double(graph_dim) - double(config.dim()); // kaess: changed to dim
  return 2. * graph.error(config) / dof; // kaess: added factor 2, graph.error returns half of actual error
}

// Global variables (these are only set once at program startup and never modified after)
string outputFile;
string inputFile;
string datasetName;
int firstStep;
int lastStep;
bool incremental;
bool batch;
bool compare;
bool perturb;
double perturbationNoise;
string compareFile1, compareFile2;

Values initial;
NonlinearFactorGraph datasetMeasurements;

// Run functions for each mode
void runIncremental();
void runBatch();
void runCompare();
void runPerturb();

/* ************************************************************************* */
int main(int argc, char *argv[]) {

  po::options_description desc("Available options");
  desc.add_options()
    ("help", "Print help message")
    ("write-solution,o", po::value<string>(&outputFile)->default_value(""), "Write graph and solution to the specified file")
    ("read-solution,i", po::value<string>(&inputFile)->default_value(""), "Read graph and solution from the specified file")
    ("dataset,d", po::value<string>(&datasetName)->default_value(""), "Read a dataset file (if and only if --incremental is used)")
    ("first-step,f", po::value<int>(&firstStep)->default_value(0), "First step to process from the dataset file")
    ("last-step,l", po::value<int>(&lastStep)->default_value(-1), "Last step to process, or -1 to process until the end of the dataset")
    ("incremental", "Run in incremental mode using ISAM2 (default)")
    ("batch", "Run in batch mode, requires an initialization from --read-solution")
    ("compare", po::value<vector<string> >()->multitoken(), "Compare two solution files")
    ("perturb", po::value<double>(&perturbationNoise), "Perturb a solution file with the specified noise")
    ;
  po::variables_map vm;
  po::store(po::command_line_parser(argc, argv).options(desc).run(), vm);
  po::notify(vm);

  batch = (vm.count("batch") > 0);
  compare = (vm.count("compare") > 0);
  perturb = (vm.count("perturb") > 0);
  incremental = (vm.count("incremental") > 0 || (!batch && !compare && !perturb));
  if(compare) {
    const vector<string>& compareFiles = vm["compare"].as<vector<string> >();
    if(compareFiles.size() != 2) {
      cout << "Must specify two files with --compare";
      exit(1);
    }
    compareFile1 = compareFiles[0];
    compareFile2 = compareFiles[1];
  }

  if((batch && incremental) || (batch && compare) || (incremental && compare)) {
    cout << "Only one of --incremental, --batch, and --compare may be specified\n" << desc << endl;
    exit(1);
  }
  if((incremental || batch) && datasetName.empty()) {
    cout << "In incremental and batch modes, a dataset must be specified\n" << desc << endl;
    exit(1);
  }
  if(!(incremental || batch) && !datasetName.empty()) {
    cout << "A dataset may only be specified in incremental or batch modes\n" << desc << endl;
    exit(1);
  }
  if(batch && inputFile.empty()) {
    cout << "In batch model, an input file must be specified\n" << desc << endl;
    exit(1);
  }
  if(perturb && (inputFile.empty() || outputFile.empty())) {
    cout << "In perturb mode, specify input and output files\n" << desc << endl;
    exit(1);
  }

  // Read input file
  if(!inputFile.empty())
  {
    cout << "Reading input file " << inputFile << endl;
    std::ifstream readerStream(inputFile.c_str(), ios::binary);
    boost::archive::binary_iarchive reader(readerStream);
    reader >> initial;
  }

  // Read dataset
  if(!datasetName.empty())
  {
    cout << "Loading dataset " << datasetName << endl;
    try {
      string datasetFile = findExampleDataFile(datasetName);
      std::pair<NonlinearFactorGraph::shared_ptr, Values::shared_ptr> data =
        load2D(datasetFile);
      datasetMeasurements = *data.first;
    } catch(std::exception& e) {
      cout << e.what() << endl;
      exit(1);
    }
  }


  // Run mode
  if(incremental)
    runIncremental();
  else if(batch)
    runBatch();
  else if(compare)
    runCompare();
  else if(perturb)
    runPerturb();

  return 0;
}

/* ************************************************************************* */
void runIncremental()
{
  ISAM2 isam2;

  // Look for the first measurement to use
  cout << "Looking for first measurement from step " << firstStep << endl;
  size_t nextMeasurement = 0;
  bool havePreviousPose = false;
  Key firstPose;
  while(nextMeasurement < datasetMeasurements.size())
  {
    if(BetweenFactor<Pose>::shared_ptr measurement =
      boost::dynamic_pointer_cast<BetweenFactor<Pose> >(datasetMeasurements[nextMeasurement]))
    {
      Key key1 = measurement->key1(), key2 = measurement->key2();
      if((key1 >= firstStep && key1 < key2) || (key2 >= firstStep && key2 < key1)) {
        // We found an odometry starting at firstStep
        firstPose = std::min(key1, key2);
        break;
      }
      if((key2 >= firstStep && key1 < key2) || (key1 >= firstStep && key2 < key1)) {
        // We found an odometry joining firstStep with a previous pose
        havePreviousPose = true;
        firstPose = std::max(key1, key2);
        break;
      }
    }
    ++ nextMeasurement;
  }

  if(nextMeasurement == datasetMeasurements.size()) {
    cout << "The supplied first step is past the end of the dataset" << endl;
    exit(1);
  }

  // If we didn't find an odometry linking to a previous pose, create a first pose and a prior
  if(!havePreviousPose) {
    cout << "Looks like " << firstPose << " is the first time step, so adding a prior on it" << endl;
    NonlinearFactorGraph newFactors;
    Values newVariables;

    newFactors.push_back(boost::make_shared<PriorFactor<Pose> >(firstPose, Pose(), noiseModel::Unit::Create(Pose::Dim())));
    newVariables.insert(firstPose, Pose());

    isam2.update(newFactors, newVariables);
  }

  cout << "Playing forward time steps..." << endl;

  for(size_t step = firstPose; nextMeasurement < datasetMeasurements.size() && (lastStep == -1 || step <= lastStep); ++step)
  {
    Values newVariables;
    NonlinearFactorGraph newFactors;

    // Collect measurements and new variables for the current step
    gttic_(Collect_measurements);
    while(nextMeasurement < datasetMeasurements.size()) {

      NonlinearFactor::shared_ptr measurementf = datasetMeasurements[nextMeasurement];

      if(BetweenFactor<Pose>::shared_ptr measurement =
        boost::dynamic_pointer_cast<BetweenFactor<Pose> >(measurementf))
      {
        // Stop collecting measurements that are for future steps
        if(measurement->key1() > step || measurement->key2() > step)
          break;

        // Require that one of the nodes is the current one
        if(measurement->key1() != step && measurement->key2() != step)
          throw runtime_error("Problem in data file, out-of-sequence measurements");

        // Add a new factor
        newFactors.push_back(measurement);

        // Initialize the new variable
        if(measurement->key1() > measurement->key2()) {
          if(!newVariables.exists(measurement->key1())) { // Only need to check newVariables since loop closures come after odometry
            if(step == 1)
              newVariables.insert(measurement->key1(), measurement->measured().inverse());
            else {
              Pose prevPose = isam2.calculateEstimate<Pose>(measurement->key2());
              newVariables.insert(measurement->key1(), prevPose * measurement->measured().inverse());
            }
          }
        } else {
          if(!newVariables.exists(measurement->key1())) { // Only need to check newVariables since loop closures come after odometry
            if(step == 1)
              newVariables.insert(measurement->key2(), measurement->measured());
            else {
              Pose prevPose = isam2.calculateEstimate<Pose>(measurement->key1());
              newVariables.insert(measurement->key2(), prevPose * measurement->measured());
            }
          }
        }
      }
      else if(BearingRangeFactor<Pose, Point2>::shared_ptr measurement =
        boost::dynamic_pointer_cast<BearingRangeFactor<Pose, Point2> >(measurementf))
      {
        Key poseKey = measurement->keys()[0], lmKey = measurement->keys()[1];

        // Stop collecting measurements that are for future steps
        if(poseKey > step)
          throw runtime_error("Problem in data file, out-of-sequence measurements");

        // Add new factor
        newFactors.push_back(measurement);

        // Initialize new landmark
        if(!isam2.getLinearizationPoint().exists(lmKey))
        {
          Pose pose;
          if(isam2.getLinearizationPoint().exists(poseKey))
            pose = isam2.calculateEstimate<Pose>(poseKey);
          else
            pose = newVariables.at<Pose>(poseKey);
          Rot2 measuredBearing = measurement->measured().first;
          double measuredRange = measurement->measured().second;
          newVariables.insert(lmKey, 
            pose.transform_from(measuredBearing.rotate(Point2(measuredRange, 0.0))));
        }
      }
      else
      {
        throw std::runtime_error("Unknown factor type read from data file");
      }
      ++ nextMeasurement;
    }
    gttoc_(Collect_measurements);

    // Update iSAM2
    gttic_(Update_ISAM2);
    isam2.update(newFactors, newVariables);
    gttoc_(Update_ISAM2);

    if((step - firstPose) % 100 == 0) {
      gttic_(chi2);
      Values estimate(isam2.calculateEstimate());
      double chi2 = chi2_red(isam2.getFactorsUnsafe(), estimate);
      cout << "chi2 = " << chi2 << endl;
      gttoc_(chi2);
    }

    tictoc_finishedIteration_();

    if((step - firstPose) % 1000 == 0) {
      cout << "Step " << step << endl;
      tictoc_print_();
    }
  }

  if(!outputFile.empty())
  {
    try {
      cout << "Writing output file " << outputFile << endl;
      std::ofstream writerStream(outputFile.c_str(), ios::binary);
      boost::archive::binary_oarchive writer(writerStream);
      Values estimates = isam2.calculateEstimate();
      writer << estimates;
    } catch(std::exception& e) {
      cout << e.what() << endl;
      exit(1);
    }
  }

  tictoc_print_();

  // Compute marginals
  //try {
  //  Marginals marginals(graph, values);
  //  int i=0;
  //  BOOST_REVERSE_FOREACH(Key key1, values.keys()) {
  //    int j=0;
  //    BOOST_REVERSE_FOREACH(Key key2, values.keys()) {
  //      if(i != j) {
  //        gttic_(jointMarginalInformation);
  //        std::vector<Key> keys(2);
  //        keys[0] = key1;
  //        keys[1] = key2;
  //        JointMarginal info = marginals.jointMarginalInformation(keys);
  //        gttoc_(jointMarginalInformation);
  //        tictoc_finishedIteration_();
  //      }
  //      ++j;
  //      if(j >= 50)
  //        break;
  //    }
  //    ++i;
  //    if(i >= 50)
  //      break;
  //  }
  //  tictoc_print_();
  //  BOOST_FOREACH(Key key, values.keys()) {
  //    gttic_(marginalInformation);
  //    Matrix info = marginals.marginalInformation(key);
  //    gttoc_(marginalInformation);
  //    tictoc_finishedIteration_();
  //    ++i;
  //  }
  //} catch(std::exception& e) {
  //  cout << e.what() << endl;
  //}
  //tictoc_print_();
}

/* ************************************************************************* */
void runBatch()
{
  cout << "Creating batch optimizer..." << endl;

  NonlinearFactorGraph measurements = datasetMeasurements;
  measurements.push_back(boost::make_shared<PriorFactor<Pose> >(0, Pose(), noiseModel::Unit::Create(Pose::Dim())));

  gttic_(Create_optimizer);
  GaussNewtonParams params;
  params.linearSolverType = SuccessiveLinearizationParams::MULTIFRONTAL_QR;
  GaussNewtonOptimizer optimizer(measurements, initial, params);
  gttoc_(Create_optimizer);
  double lastError;
  do {
    lastError = optimizer.error();
    gttic_(Iterate_optimizer);
    optimizer.iterate();
    gttoc_(Iterate_optimizer);
    cout << "Error: " << lastError << " -> " << optimizer.error() /*<< ", lambda: " << optimizer.lambda()*/ << endl;
    gttic_(chi2);
    double chi2 = chi2_red(measurements, optimizer.values());
    cout << "chi2 = " << chi2 << endl;
    gttoc_(chi2);
  } while(!checkConvergence(optimizer.params().relativeErrorTol,
    optimizer.params().absoluteErrorTol, optimizer.params().errorTol,
    lastError, optimizer.error(), optimizer.params().verbosity));

  tictoc_finishedIteration_();
  tictoc_print_();

  if(!outputFile.empty())
  {
    try {
      cout << "Writing output file " << outputFile << endl;
      std::ofstream writerStream(outputFile.c_str(), ios::binary);
      boost::archive::binary_oarchive writer(writerStream);
      writer << optimizer.values();
    } catch(std::exception& e) {
      cout << e.what() << endl;
      exit(1);
    }
  }
}

/* ************************************************************************* */
void runCompare()
{
  Values soln1, soln2;

  cout << "Reading solution file " << compareFile1 << endl;
  {
    std::ifstream readerStream(compareFile1.c_str(), ios::binary);
    boost::archive::binary_iarchive reader(readerStream);
    reader >> soln1;
  }

  cout << "Reading solution file " << compareFile2 << endl;
  {
    std::ifstream readerStream(compareFile2.c_str(), ios::binary);
    boost::archive::binary_iarchive reader(readerStream);
    reader >> soln2;
  }

  // Check solution for equality
  cout << "Comparing solutions..." << endl;
  vector<Key> missingKeys;
  br::set_symmetric_difference(soln1.keys(), soln2.keys(), std::back_inserter(missingKeys));
  if(!missingKeys.empty()) {
    cout << "  Keys unique to one solution file: ";
    for(size_t i = 0; i < missingKeys.size(); ++i) {
      cout << DefaultKeyFormatter(missingKeys[i]);
      if(i != missingKeys.size() - 1)
        cout << ", ";
    }
    cout << endl;
  }
  vector<Key> commonKeys;
  br::set_intersection(soln1.keys(), soln2.keys(), std::back_inserter(commonKeys));
  double maxDiff = 0.0;
  BOOST_FOREACH(Key j, commonKeys)
    maxDiff = std::max(maxDiff, soln1.at(j).localCoordinates_(soln2.at(j)).norm());
  cout << "  Maximum solution difference (norm of logmap): " << maxDiff << endl;
}

/* ************************************************************************* */
void runPerturb()
{
  // Set up random number generator
  boost::random::mt19937 rng;
  boost::random::normal_distribution<double> normal(0.0, perturbationNoise);

  // Perturb values
  VectorValues noise;
  Ordering ordering = *initial.orderingArbitrary();
  BOOST_FOREACH(const Values::KeyValuePair& key_val, initial)
  {
    Vector noisev(key_val.value.dim());
    for(Vector::Index i = 0; i < noisev.size(); ++i)
      noisev(i) = normal(rng);
    noise.insert(ordering[key_val.key], noisev);
  }
  Values perturbed = initial.retract(noise, ordering);

  // Write results
  try {
    cout << "Writing output file " << outputFile << endl;
    std::ofstream writerStream(outputFile.c_str(), ios::binary);
    boost::archive::binary_oarchive writer(writerStream);
    writer << perturbed;
  } catch(std::exception& e) {
    cout << e.what() << endl;
    exit(1);
  }

}

