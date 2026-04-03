# ------------------------------------------------------------
# Minimal protocol for DSL performance benchmarking
# ------------------------------------------------------------

# Small message: tests string encoding/decoding
message SmallMessage (id=1, version=1)
    string name
    i32    value
end

# Medium message: tests list-of-strings performance
message MediumMessage (id=2, version=1)
    list<string> tags
    i64          sequence
end

