#include "../ContractRegistry.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace retrovue::tests
{
namespace
{

std::map<std::string, std::vector<std::string>> ExpectedCoverage()
{
  return {
      {"MasterClock",
       {"MC-001",
        "MC-002",
        "MC-003",
        "MC-004",
        "MC-005",
        "MC-006"}},
      {"MetricsAndTiming",
       {"MT-001",
        "MT-002",
        "MT-003",
        "MT-004",
        "MT-005",
        "MT-006",
        "MT-007",
        "MT-008"}},
      {"MetricsExport",
       {"MET-001",
        "MET-002",
        "MET-003"}},
      {"PlayoutEngine",
       {"BC-001",
        "BC-002",
        "BC-003",
        "BC-004",
        "BC-005",
        "BC-006"}},
      {"Renderer",
       {"FE-001",
        "FE-002",
        "FE-003",
        "FE-004",
        "FE-005"}},
      {"VideoFileProducer",
       {"FE-001",
        "FE-002",
        "FE-003",
        "FE-004",
        "FE-005",
        "FE-006",
        "FE-007",
        "FE-008",
        "FE-009",
        "FE-010",
        "FE-011",
        "FE-012"}}};
}

TEST(ContractRegistry, AllRulesCovered)
{
  const auto& registry = ContractRegistry::Instance();
  for (const auto& [domain, expected_rules] : ExpectedCoverage())
  {
    const auto covered_rules = registry.CoveredRules(domain);
    if (covered_rules.empty())
    {
      SCOPED_TRACE("Domain '" + domain +
                   "' has no registered contract suites in this test binary.");
      continue;
    }

    const std::set<std::string> expected(expected_rules.begin(),
                                         expected_rules.end());

    std::vector<std::string> missing;
    std::set_difference(expected.begin(),
                        expected.end(),
                        covered_rules.begin(),
                        covered_rules.end(),
                        std::back_inserter(missing));

    std::vector<std::string> unexpected;
    std::set_difference(covered_rules.begin(),
                        covered_rules.end(),
                        expected.begin(),
                        expected.end(),
                        std::back_inserter(unexpected));

    if (!missing.empty() || !unexpected.empty())
    {
      ADD_FAILURE() << "Contract coverage mismatch for domain '" << domain
                    << "'. Missing: " << ::testing::PrintToString(missing)
                    << " Unexpected: " << ::testing::PrintToString(unexpected);
    }
  }
}

} // namespace
} // namespace retrovue::tests


