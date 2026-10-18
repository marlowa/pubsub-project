# Every kind of field the DSL has, for checking each code generator byte for byte against the C++ one
# (python/tests/test_generator_python.py). Not used by the venue. A list of enums is left out: the C++ generator
# cannot yet write one (BUG-0177).

enum Small : i8 {
    Low = -128
    Zero = 0
    High = 127
}

enum Medium : i16 {
    Least = -32768
    Most = 32767
}

enum Large : i32 {
    Least = -2147483648
    One = 1
    Most = 2147483647
}

enum Huge : i64 {
    Least = -9223372036854775808
    Most = 9223372036854775807
}

enum Letter : char {
    Buy = '1'
    Sell = '2'
}

message Integers (id=1)
    i8 tiny
    i16 short_number
    i32 number
    i64 wide
    datetime_ns when
    bool flag
    char letter
end

message Text (id=2)
    string name
    bytes blob
end

message Enums (id=3)
    Small small
    Medium medium
    Large large
    Huge huge
    Letter letter
end

message Inner (id=4)
    i32 number
    string label
end

message Lists (id=5)
    list<i32> numbers
    list<string> names
    list<bytes> blobs
    list<Inner> inners
    list<list<i16>> grid
    list<list<string>> words
end

message Arrays (id=6)
    i8[4] small_array
    i16[2] pair
    i32[3] numbers
    i64[2] wide
end

message Nested (id=7)
    Inner inner
    optional Inner maybe_inner
    i32 after
end

message Optionals (id=8)
    i32 required
    optional i8 tiny
    optional i64 wide
    optional bool flag
    optional string name
    optional bytes blob
    optional Small small
    optional list<i32> numbers
end
