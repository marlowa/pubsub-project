enum status_code : i32 {
    ok = 0
    warning = 1
    error = 2
}

message CpuSample (id=10, version=1)
    i64 timestamp_ns
    i32 core
    i32 user_pct
    i32 system_pct
    i32 idle_pct
end

message ProcessInfo (id=11, version=1)
    i64 pid
    string name
    array<i8>[16] hash
    optional status_code status
end

message TelemetryPacket (id=12, version=1)
    datetime_ns captured_at
    list<CpuSample> cpu
    list<ProcessInfo> processes
end
