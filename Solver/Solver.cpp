#include "Solver.h"

#include <algorithm>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <mutex>

#include <cmath>

#include "CsvReader.h"


using namespace std;


namespace szx {

#pragma region Solver::Cli
int Solver::Cli::run(int argc, char * argv[]) {
    Log(LogSwitch::Szx::Cli) << "parse command line arguments." << endl;
    Set<String> switchSet;
    Map<String, char*> optionMap({ // use string as key to compare string contents instead of pointers.
        { InstancePathOption(), nullptr },
        { SolutionPathOption(), nullptr },
        { RandSeedOption(), nullptr },
        { TimeoutOption(), nullptr },
        { MaxIterOption(), nullptr },
        { JobNumOption(), nullptr },
        { RunIdOption(), nullptr },
        { EnvironmentPathOption(), nullptr },
        { ConfigPathOption(), nullptr },
        { LogPathOption(), nullptr }
    });

    for (int i = 1; i < argc; ++i) { // skip executable name.
        auto mapIter = optionMap.find(argv[i]);
        if (mapIter != optionMap.end()) { // option argument.
            mapIter->second = argv[++i];
        } else { // switch argument.
            switchSet.insert(argv[i]);
        }
    }

    Log(LogSwitch::Szx::Cli) << "execute commands." << endl;
    if (switchSet.find(HelpSwitch()) != switchSet.end()) {
        cout << HelpInfo() << endl;
    }

    if (switchSet.find(AuthorNameSwitch()) != switchSet.end()) {
        cout << AuthorName() << endl;
    }

    Solver::Environment env;
    env.load(optionMap);
    if (env.instPath.empty() || env.slnPath.empty()) { return -1; }

    Solver::Configuration cfg;
    cfg.load(env.cfgPath);

    Log(LogSwitch::Szx::Input) << "load instance " << env.instPath << " (seed=" << env.randSeed << ")." << endl;
    Problem::Input input;
    if (!input.load(env.instPath)) { return -1; }

    Solver solver(input, env, cfg);
    solver.solve();

    pb::Submission submission;
    submission.set_thread(to_string(env.jobNum));
    submission.set_instance(env.friendlyInstName());
    submission.set_duration(to_string(solver.timer.elapsedSeconds()) + "s");
    submission.set_obj(solver.output.totalCost);

    solver.output.save(env.slnPath, submission);
    #if QYM_DEBUG
    solver.output.save(env.solutionPathWithTime(), submission);
    solver.record();
    #endif // QYM_DEBUG

    return 0;
}
#pragma endregion Solver::Cli

#pragma region Solver::Environment
void Solver::Environment::load(const Map<String, char*> &optionMap) {
    char *str;

    str = optionMap.at(Cli::EnvironmentPathOption());
    if (str != nullptr) { loadWithoutCalibrate(str); }

    str = optionMap.at(Cli::InstancePathOption());
    if (str != nullptr) { instPath = str; }

    str = optionMap.at(Cli::SolutionPathOption());
    if (str != nullptr) { slnPath = str; }

    str = optionMap.at(Cli::RandSeedOption());
    if (str != nullptr) { randSeed = atoi(str); }

    str = optionMap.at(Cli::TimeoutOption());
    if (str != nullptr) { msTimeout = static_cast<Duration>(atof(str) * Timer::MillisecondsPerSecond); }

    str = optionMap.at(Cli::MaxIterOption());
    if (str != nullptr) { maxIter = atoi(str); }

    str = optionMap.at(Cli::JobNumOption());
    if (str != nullptr) { jobNum = atoi(str); }

    str = optionMap.at(Cli::RunIdOption());
    if (str != nullptr) { rid = str; }

    str = optionMap.at(Cli::ConfigPathOption());
    if (str != nullptr) { cfgPath = str; }

    str = optionMap.at(Cli::LogPathOption());
    if (str != nullptr) { logPath = str; }

    calibrate();
}

void Solver::Environment::load(const String &filePath) {
    loadWithoutCalibrate(filePath);
    calibrate();
}

void Solver::Environment::loadWithoutCalibrate(const String &filePath) {
    // EXTEND[qym][8]: load environment from file.
    // EXTEND[qym][8]: check file existence first.
}

void Solver::Environment::save(const String &filePath) const {
    // EXTEND[qym][8]: save environment to file.
}
void Solver::Environment::calibrate() {
    // adjust thread number.
    int threadNum = thread::hardware_concurrency();
    if ((jobNum <= 0) || (jobNum > threadNum)) { jobNum = threadNum; }

    // adjust timeout.
    msTimeout -= Environment::SaveSolutionTimeInMillisecond;
}
#pragma endregion Solver::Environment

#pragma region Solver::Configuration
void Solver::Configuration::load(const String &filePath) {
    // EXTEND[szx][5]: load configuration from file.
    // EXTEND[szx][8]: check file existence first.
}

void Solver::Configuration::save(const String &filePath) const {
    // EXTEND[szx][5]: save configuration to file.
}
#pragma endregion Solver::Configuration

#pragma region Solver
bool Solver::solve() {
    init();

    int workerNum = (max)(1, env.jobNum / cfg.threadNumPerWorker);
    cfg.threadNumPerWorker = env.jobNum / workerNum;
    List<Solution> solutions(workerNum, Solution(this));
    List<bool> success(workerNum);

    Log(LogSwitch::Szx::Framework) << "launch " << workerNum << " workers." << endl;
    List<thread> threadList;
    threadList.reserve(workerNum);
    for (int i = 0; i < workerNum; ++i) {
        // TODO[szx][2]: as *this is captured by ref, the solver should support concurrency itself, i.e., data members should be read-only or independent for each worker.
        // OPTIMIZE[szx][3]: add a list to specify a series of algorithm to be used by each threads in sequence.
        threadList.emplace_back([&, i]() { success[i] = optimize(solutions[i], i); });
    }
    for (int i = 0; i < workerNum; ++i) { threadList.at(i).join(); }

    Log(LogSwitch::Szx::Framework) << "collect best result among all workers." << endl;
    int bestIndex = -1;
    double bestValue = 0;
    for (int i = 0; i < workerNum; ++i) {
        if (!success[i]) { continue; }
        Log(LogSwitch::Szx::Framework) << "worker " << i << " got " << solutions[i].totalCost << endl;
        if (solutions[i].totalCost <= bestValue) { continue; }
        bestIndex = i;
        bestValue = solutions[i].totalCost;
    }

    env.rid = to_string(bestIndex);
    if (bestIndex < 0) { return false; }
    output = solutions[bestIndex];
    return true;
}

void Solver::record() const {
    #if QYM_DEBUG
    int generation = 0;

    ostringstream log;

    System::MemoryUsage mu = System::peakMemoryUsage();

    // load reference results.
    CsvReader cr;
    ifstream ifs(Environment::DefaultInstanceDir() + "Baseline.csv");
    if (!ifs.is_open()) { return; }
    const List<CsvReader::Row> &rows(cr.scan(ifs));
    ifs.close();
    String bestObj, refObj, refTime;
    for (auto r = rows.begin(); r != rows.end(); ++r) {
        if (env.friendlyInstName() != r->front()) { continue; }
        bestObj = (*r)[1];
        refObj = (*r)[2];
        refTime = (*r)[3];
        //double opt = stod(bestObj);
        break;
    }

    double checkerObj = -1;
    bool feasible = check(checkerObj);
    double objDiff = round(output.totalCost * Problem::CheckerObjScale - checkerObj) / Problem::CheckerObjScale;

    // record basic information.
    log << env.friendlyLocalTime() << ","
        << env.rid << ","
        << env.instPath << ","
        << feasible << "," << objDiff << ","
        << output.totalCost << ","
        << bestObj << ","
        << refObj << ","
        << timer.elapsedSeconds() << ","
        << refTime << ","
        << mu.physicalMemory << "," << mu.virtualMemory << ","
        << env.randSeed << ","
        << cfg.toBriefStr() << ","
        << generation << "," << iteration;

    // record solution vector.
    // EXTEND[qym][2]: save solution in log.
    log << endl;

    // append all text atomically.
    static mutex logFileMutex;
    lock_guard<mutex> logFileGuard(logFileMutex);

    ofstream logFile(env.logPath, ios::app);
    logFile.seekp(0, ios::end);
    if (logFile.tellp() <= 0) {
        logFile << "Time,ID,Instance,Feasible,ObjMatch,Cost,MinCost,RefCost,Duration,RefDuration,PhysMem,VirtMem,RandSeed,Config,Generation,Iteration,Solution" << endl;
    }
    logFile << log.str();
    logFile.close();
    #endif // QYM_DEBUG
}

bool Solver::check(double &checkerObj) const {
    #if QYM_DEBUG
    enum CheckerFlag {
        IoError = 0x0,
        FormatError = 0x1,
        MultipleVisitsError = 0x2,
        UnmatchedLoadDeliveryError = 0x4,
        ExceedCapacityError = 0x8,
        RunOutOfStockError = 0x10
    };

    int errorCode = System::exec("Checker.exe " + env.instPath + " " + env.solutionPathWithTime());
    if (errorCode > 0) {
        checkerObj = errorCode;
        return true;
    }
    errorCode = ~errorCode;
    if (errorCode == CheckerFlag::IoError) { Log(LogSwitch::Checker) << "IoError." << endl; }
    if (errorCode & CheckerFlag::FormatError) { Log(LogSwitch::Checker) << "FormatError." << endl; }
    if (errorCode & CheckerFlag::MultipleVisitsError) { Log(LogSwitch::Checker) << "MultipleVisitsError." << endl; }
    if (errorCode & CheckerFlag::UnmatchedLoadDeliveryError) { Log(LogSwitch::Checker) << "UnmatchedLoadDeliveryError." << endl; }
    if (errorCode & CheckerFlag::ExceedCapacityError) { Log(LogSwitch::Checker) << "ExceedCapacityError." << endl; }
    if (errorCode & CheckerFlag::RunOutOfStockError) { Log(LogSwitch::Checker) << "RunOutOfStockError." << endl; }
    return false;
    #else
    checkerObj = 0;
    return true;
    #endif // QYM_DEBUG
}

void Solver::init() {
    aux.routingCost.init(input.nodes_size(), input.nodes_size());
    aux.routingCost.reset(Arr2D<Price>::ResetOption::AllBits0);
    ID n = 0;
    for (auto i = input.nodes().begin(); i != input.nodes().end(); ++i, ++n) {
        ID m = 0;
        for (auto j = input.nodes().begin(); j != i; ++j, ++m) {
            double value = round(hypot(i->x() - j->x(), i->y() - j->y()));
            aux.routingCost[n][m] = aux.routingCost[m][n] = value;
        }
    }
}

bool Solver::optimize(Solution &sln, ID workerId) {
    Log(LogSwitch::Szx::Framework) << "worker " << workerId << " starts." << endl;

    ID nodeNum = input.nodes_size();
    ID vehicleNum = input.vehicles_size();
    const auto &vehicles(*input.mutable_vehicles());
    const auto &nodes(*input.mutable_nodes());

    // reset solution state.
    bool status = true;

    sln.totalCost = 0;
    List<Quantity> restQuantity(nodeNum, 0);
    ID n = 0;
    for (auto i = input.nodes().begin(); i != input.nodes().end(); ++i, ++n) {
        restQuantity[n] = i->initquantity();
        sln.totalCost += i->holdingcost() * restQuantity[n];
    }

    // TODO[0]: replace the following random assignment with your own algorithm.
    for (ID p = 0; p < input.periodnum(); ++p) {
        auto &periodRoute(*sln.add_periodroutes());
        for (auto v = vehicles.begin(); v != vehicles.end(); ++v) {
            auto &route(*periodRoute.add_vehicleroutes());

            int visitedCustomerNum = rand.pick(nodeNum);
            if (visitedCustomerNum <= 0) { continue; }
            int totalQuantity = 0;
            int prevNode = 0;
            for (int i = 0; i < visitedCustomerNum; ++i) {
                auto &delivery(*route.add_deliveries());
                delivery.set_node(rand.pick(input.depotnum(), nodeNum)); // may violate single visit constraint.
                delivery.set_quantity(rand.pick(v->capacity() + 1)); // may violate vehicle/node capacity constraint.
                totalQuantity += delivery.quantity();
                restQuantity[delivery.node()] += delivery.quantity();
                sln.totalCost += aux.routingCost[prevNode][delivery.node()];
                prevNode = delivery.node();
            }

            // the depot should be the destination of the tour.
            auto &delivery(*route.add_deliveries());
            delivery.set_node(0);
            delivery.set_quantity(-totalQuantity);
            restQuantity[0] += delivery.quantity();
            sln.totalCost += aux.routingCost[prevNode][0];
        }
        ID n = 0;
        for (auto i = nodes.begin(); i != nodes.end(); ++i, ++n) {
            restQuantity[n] -= i->demands(p);
            sln.totalCost += restQuantity[n] * i->holdingcost();
        }
    }
        
    Log(LogSwitch::Szx::Framework) << "worker " << workerId << " ends." << endl;
    return status;
}
#pragma endregion Solver

}
