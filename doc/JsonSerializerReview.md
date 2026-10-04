# AWL JSON serializer review

Scope: the serializers included by `BoostExtras/Json/Json.h`, JSON passthrough
types, range helpers and `JsonableObject`. The findings below record the original
review. The implementation status has since changed while expanding TypedTool
schemas:

- Wide strings now use checked UTF-8 conversion, including surrogate validation.
- Integer double input is checked before casting, unsigned values avoid a signed
  intermediate, and character integer types parse strings through 64-bit integers.
- Time points retain epoch milliseconds, with checked range/precision on input,
  complete integer-string parsing, support for coarse duration types and checked
  output conversion. Output still truncates sub-millisecond ticks.
- List/deque parsing no longer requires `reserve`; multimaps can parse object
  keys, and duplicate output map keys are rejected instead of losing values.
- TypedTool rejects non-finite values and invalid UTF-8 throughout the document,
  validates exact tuple lengths and reflected fields, and validates decimal and
  duration strings via their serializers. Opaque DOM fields keep their number kinds.

Duration keeps the AWL extended string format and delegates to the common AWL
helpers. Floating point coercion, decimal numeric coercion and permissive raw
AWL tuple/reflection parsing remain as described below; TypedTool uses stricter
input validation. This layer is not a general JSON Schema interpreter.

RFC 8259 defines JSON syntax, Unicode strings and finite numeric representations.
It does not define C++ type mappings, dates, enums or decimals. A numeric string,
enum name, decimal string or timestamp number is not inherently invalid JSON.
[RFC 8259](https://www.rfc-editor.org/rfc/rfc8259.html)

## Findings

1. **Wide strings corrupt Unicode.** `JsonString.h` converts between `string`
   and `wstring` by copying and narrowing individual code units. This is not
   UTF-8 conversion. A Windows/MSVC probe serialized `L"\u0416"` as
   `"\u0016"`; decoding the UTF-8 bytes `D0 96` produced two wide characters
   instead of one. Use explicit UTF-8 encoding/decoding, including surrogate
   validation and embedded zero handling. AWL's `encodeString`/`decodeString`
   use the C locale, so they are not automatically suitable replacements.
   JSON exchanged between systems must use UTF-8.
   [RFC 8259, section 8.1](https://www.rfc-editor.org/rfc/rfc8259.html#section-8.1)

2. **Floating point serialization accepts non-finite values and unchecked
   narrowing.** `JsonArithmetic.h` casts any floating point value to `double`
   without checking finiteness, range or precision. With Boost.JSON 1.89 defaults,
   the probe serialized infinity as `1e99999` and NaN as `null`. The resulting
   text uses legal JSON syntax, but it changes the value/type and will not round
   trip as an ordinary finite number. Reject non-finite input at the AWL boundary;
   check conversions to narrower floating point types. JSON numeric literals
   `Infinity` and `NaN` are not allowed by RFC 8259.
   [RFC 8259, section 6](https://www.rfc-editor.org/rfc/rfc8259.html#section-6)

3. **Several numeric input conversions lack range checks before casting.**
   Integer deserialization casts a JSON `double` to `int64_t` before proving
   that it is finite and representable. This can have undefined behavior;
   it also routes unsigned 64-bit targets through a signed type. Floating point
   string parsing uses `strtod` without checking `errno`, non-finite results or
   whether any digits were consumed: the probe accepted an empty string as zero
   and `"1e1000"` as infinity. Numeric string coercion itself is an AWL policy,
   not an RFC violation; the unchecked conversions are the problem.

4. **`time_point` uses epoch milliseconds, not RFC 3339 date-time strings.**
   `JsonTime.h` converts input through `double`, truncates fractional
   milliseconds, casts without range checks and accepts numeric prefixes via
   `stod`: the probe accepted `"123junk"` as 123 milliseconds. Writing truncates
   sub-millisecond precision. The generic clock parameter also allows clocks
   without a meaningful UTC epoch; a `time_point` with a coarser duration such
   as seconds cannot use the current millisecond constructor in `fromJson`.
   Epoch milliseconds are valid JSON, but are a different API contract from
   RFC 3339. A future RFC 3339 serializer needs an explicit UTC clock mapping,
   offset parsing and a policy for precision/leap seconds.
   [RFC 3339, section 5.6](https://www.rfc-editor.org/rfc/rfc3339.html#section-5.6)

5. **Decimal numeric input can lose precision.** `JsonDecimal.h` serializes
   decimals as strings, which preserves decimal precision. However, all numeric
   input passes through `double`, including 64-bit integers, then through an
   unchecked `int64_t` cast. Large integer inputs can lose digits or exceed the
   cast's range. Prefer separate integer conversion paths and explicit finite,
   range and precision checks for floating point input. Decimal-as-string is a
   valid JSON representation and is not prohibited by RFC 8259.

6. **Tuple/reflection input is intentionally permissive in places.**
   `JsonTuple.h` rejects short arrays but ignores extra elements; the probe
   confirmed `[1,2]` was accepted for `tuple<int>`. `JsonReflectable.h` ignores
   unknown properties and presents missing properties as null to the field
   serializer. These are mapping policies, not JSON syntax violations. Consider
   explicit strict mode where schemas require exact shapes. The MCP TypedTool
   validator already rejects unknown fields in its supported reflected objects.

## Coverage by type

| Type/group | JSON representation | Review result |
|---|---|---|
| `bool` | Boolean | Correct type check; no format issue found. |
| Integral types | Integer | Normal 64-bit paths are checked; double input needs the checks in finding 3. |
| Floating point types | Number | Findings 2 and 3. |
| `std::string` | String | Byte-preserving; callers must supply valid UTF-8. No UTF-8 validation in this wrapper. |
| `std::wstring` | String | Finding 1. |
| AWL enums | Name string | Valid mapping; invalid input names become `JsonException`. |
| AWL decimals | Decimal string | Output preserves precision; numeric input has finding 5. |
| `time_point` | Epoch-millisecond integer | Finding 4. |
| `duration` | AWL extended string | Format preserved; common format/parse helpers now check integer conversion ranges and precision. |
| `optional<T>` | Value or null | Valid mapping; missing reflected optional fields become empty. |
| Sequences and `rangeToJson` | Array | Valid mapping; element behavior depends on its serializer. Sequence parsing can partially change the destination on failure. |
| String-keyed maps | Object | Valid mapping; values depend on their serializer. Parsing can partially change the destination on failure. Duplicate textual keys must be addressed at the JSON parser boundary. |
| Tuples | Array | Finding 6; parsing can partially change fields on failure. |
| Reflected objects | Object | Finding 6; parsing can partially change fields on failure. |
| `atomic<T>`, `reference_wrapper<T>` | Representation of `T` | Delegation introduces no extra wire format. |
| Boost.JSON value/object/array | Copied JSON DOM | Passthrough; inherited non-finite/encoding values are not validated here. |
| `Jsonable`, `JsonableObject<T>` | Custom/delegated JSON | Interface/adapter only; concrete serializers define behavior. |

Initial verification: source review of the listed serializers; standalone probes built
with MSVC and Boost.JSON 1.89 for the examples above. The probes avoid executing
the potentially undefined out-of-range numeric casts. The 20 CppMcp tests pass,
including four duration tests for canonical strings, narrow/wide input, signed
nanosecond limits, overflow, loss of precision, error translation and nested JSON
round trips. The serializers have not been exhaustively fuzz-tested. Expanded
coverage adds seven tests, including `TypedToolAllTypes` over both transports and
six `JsonSchema` tests for type families, shapes, invalid inputs, wrappers, Unicode
and custom serializers.
