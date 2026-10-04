// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Slick Quant

#include <slick/perfmon.hpp>
#include <slick/perfmon/collector.hpp>
#include <slick/perfmon/version.hpp>

int main() {
    slick::perfmon::Collector::instance().flush();
    return 0;
}
