#include "ContractRegistryEnvironment.h"

#include "../ContractRegistry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace retrovue::tests
{

namespace
{

using ExpectedCoverageMap = std::map<std::string, std::vector<std::string>>;

ExpectedCoverageMap& CoverageExpectations()
{
  static ExpectedCoverageMap map;
  return map;
}

std::mutex& CoverageMutex()
{
  static std::mutex mutex;
  return mutex;
}

std::vector<std::string> Deduplicate(std::vector<std::string> values)
{
  std::set<std::string> unique(values.begin(), values.end());
  return std::vector<std::string>(unique.begin(), unique.end());
}

std::string Join(const std::vector<std::string>& values)
{
  std::ostringstream oss;
  for (std::size_t i = 0; i < values.size(); ++i)
  {
    if (i != 0)
    {
      oss << ", ";
    }
    oss << values[i];
  }
  return oss.str();
}

void VerifyDomainCoverage(const std::string& domain,
                          const std::vector<std::string>& expected_rules)
{
  const auto missing = ContractRegistry::Instance().MissingRules(domain, expected_rules);
  if (!missing.empty())
  {
    ADD_FAILURE() << "Missing contract coverage for domain '" << domain
                  << "': " << Join(missing);
  }
}

class ContractRegistryEnvironment : public ::testing::Environment
{
public:
  void TearDown() override
  {
    std::lock_guard<std::mutex> lock(CoverageMutex());
    const auto& registry = ContractRegistry::Instance();
    for (const auto& [domain, rules] : CoverageExpectations())
    {
      // Only verify coverage for domains that actually have tests registered
      // This allows filtered test runs (e.g., --gtest_filter) to work correctly
      const auto covered_rules = registry.CoveredRules(domain);
      if (!covered_rules.empty())
      {
        VerifyDomainCoverage(domain, Deduplicate(rules));
      }
    }
  }
};

::testing::Environment* const kContractRegistryEnvironment =
    ::testing::AddGlobalTestEnvironment(new ContractRegistryEnvironment());

} // namespace

void RegisterExpectedDomainCoverage(std::string domain,
                                    std::vector<std::string> rule_ids)
{
  std::lock_guard<std::mutex> lock(CoverageMutex());
  auto& map = CoverageExpectations();
  auto& rules = map[std::move(domain)];
  rules.insert(rules.end(), rule_ids.begin(), rule_ids.end());
}

} // namespace retrovue::tests

