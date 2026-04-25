cmake_minimum_required(VERSION 3.11 FATAL_ERROR)

# ---------------------------------------------------------------------------
# DSL code generation
#
# Generates fix_equity_orders.hpp from fix_equity_orders.dsl into the build
# tree. All four application targets that use FIX equity order PDUs depend
# on this target. Generated output lands in ${CMAKE_CURRENT_BINARY_DIR} which
# resolves to build/applications/ -- never in the source tree.
# ---------------------------------------------------------------------------

set(FIX_DSL_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/fix_equity_orders.dsl")
set(FIX_DSL_GENERATED_HPP "${CMAKE_CURRENT_BINARY_DIR}/fix_equity_orders.hpp")
set(FIX_DSL_GENERATOR "${CMAKE_SOURCE_DIR}/python/tools/generate_cpp_from_dsl.py")

add_custom_command(
    OUTPUT "${FIX_DSL_GENERATED_HPP}"
    COMMAND python3 "${FIX_DSL_GENERATOR}"
            --topics
            --namespace APM
            "${FIX_DSL_SOURCE}"
            "${CMAKE_CURRENT_BINARY_DIR}/fix_equity_orders.hpp"
    DEPENDS "${FIX_DSL_SOURCE}" "${FIX_DSL_GENERATOR}"
    COMMENT "Generating fix_equity_orders.hpp from fix_equity_orders.dsl"
    VERBATIM
)

add_custom_target(fix_equity_orders_generated
    DEPENDS "${FIX_DSL_GENERATED_HPP}"
)

# ---------------------------------------------------------------------------
# Applications
# ---------------------------------------------------------------------------

add_subdirectory(sample_fix_gateway)
add_subdirectory(sample_fix_gateway_seq)
add_subdirectory(sequencer)
add_subdirectory(matching_engine)
add_subdirectory(arbiter)
