cmake_minimum_required(VERSION 3.11 FATAL_ERROR)

# ---------------------------------------------------------------------------
# DSL code generation for fix_orders has been lifted to the top-level
# CMakeLists.txt so that targets in libraries/ can also depend on the
# generated header. The fix_orders_generated target and the
# FIX_DSL_INCLUDE_DIR cache variable are defined there.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Applications
# ---------------------------------------------------------------------------
add_subdirectory(order_gateway)
add_subdirectory(binary_gateway)
add_subdirectory(sequencer)
add_subdirectory(matching_engine)
add_subdirectory(matching_engine_publisher)
add_subdirectory(arbiter)
add_subdirectory(witness)
add_subdirectory(authentication_service)
add_subdirectory(topic_probe)
