// StdOutputDriver.h
// Implements the standard test_driver output system

#include "TestOutputDriver.h"

#include <map>
#include <string>

class StdOutputDriver : public TestOutputDriver {
private:
  std::map<TestOutputStream, std::string> streams;
  std::map<std::string, std::string> *attributes;
  TestInfo *last_test;
  RunGroup *last_group;

public:
  TESTLIB_DLL_EXPORT StdOutputDriver(void * data);
  ~StdOutputDriver();

  virtual void startNewTest(std::map<std::string, std::string> &attributes, TestInfo *test, RunGroup *group);

  virtual void redirectStream(TestOutputStream stream, const char * filename);
  virtual void logResult(test_results_t result, int stage=-1);
  virtual void logCrash(std::string testname);
  virtual void log(TestOutputStream stream, const char *fmt, ...);
  virtual void vlog(TestOutputStream stream, const char *fmt, va_list args);
  virtual void finalizeOutput();
};