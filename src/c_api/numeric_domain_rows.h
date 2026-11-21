#pragma once

/// @file numeric_domain_rows.h
/// @brief Every C-ABI (entry, parameter) whose refusal reads a named NumericDomain.
/// @details The partitions are the source of tools/api/numeric-domains.json. They hold only inline
///          and constexpr content, so a tool can include them and print them without linking the
///          library.

#include "c_api/numeric_domain_row.h"
#include "c_api/numeric_domain_rows_analysis.h"
#include "c_api/numeric_domain_rows_effects.h"
#include "c_api/numeric_domain_rows_instruments.h"
#include "c_api/numeric_domain_rows_mastering.h"
#include "c_api/numeric_domain_rows_project.h"

namespace sonare_c_detail {

inline const NumericDomainTable kNumericDomainTables[] = {
    numeric_domain_table("analysis", kAnalysisDomainRows),
    numeric_domain_table("effects", kEffectsDomainRows),
    numeric_domain_table("instruments", kInstrumentDomainRows),
    numeric_domain_table("mastering", kMasteringDomainRows),
    numeric_domain_table("project", kProjectDomainRows),
};

}  // namespace sonare_c_detail
