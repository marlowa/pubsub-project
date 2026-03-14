enum Venue : i16 {
    lse = 1
    chix = 2
    bats = 3
}

message Trade (id=1, version=1)
    i64 price
    i32 quantity
    Venue venue
end

message PriceUpdate (id=2, version=1)
    i64 instrument_id
    Venue venue
    string source
    list<Trade> trades
    optional string comment
    optional i64 sequence_number
end
