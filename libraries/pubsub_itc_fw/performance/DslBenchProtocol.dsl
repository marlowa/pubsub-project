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

# Large message: tests nested list performance
message LargeMessage (id=3, version=1)
    list<list<string>> groups
    i64                sequence
    list<MediumMessage> items
end
