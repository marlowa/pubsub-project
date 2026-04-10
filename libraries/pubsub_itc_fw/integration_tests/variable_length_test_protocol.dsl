# ============================================================
#  Integration Test Protocol — Variable-Length PDU Definitions
# ============================================================
#
#  This DSL file exists solely to exercise the variable-length
#  encoding and decoding paths in the integration test suite.
#  It is not part of any production protocol.
#
#  It deliberately includes:
#    - A string field           (variable-length)
#    - An optional i32 field    (presence flag + value)
#    - A list<string> field     (variable-length list of
#                                variable-length elements)
#
#  These are the three cases that require arena allocation
#  during decode, unlike the fixed-size leader-follower PDUs.
#
# ------------------------------------------------------------

# ------------------------------------------------------------
#  300 — DataQuery
#  Sent by the connector to the listener.
#  Carries a query name (string), an optional result limit,
#  and a correlation ID to be echoed back in the response.
# ------------------------------------------------------------
message DataQuery (id=300, version=1)
    i64 request_id             # correlation ID echoed in DataResponse
    string query_name          # the query to execute (variable length)
    optional i32 limit         # optional maximum number of results
end

# ------------------------------------------------------------
#  301 — DataResponse
#  Sent by the listener back to the connector.
#  Carries the echoed correlation ID, a status code, and a
#  list of result strings.
# ------------------------------------------------------------
message DataResponse (id=301, version=1)
    i64 request_id             # echoed from DataQuery
    i32 status_code            # 0 = success
    list<string> results       # variable-length list of result strings
end
