//===----------------------------------------------------------------------===//
//                                                                            //
//    ______    _                                                             //
//   /_  __/___(_)_  __                                                       //
//    / / / __/ /\ \/ /       Stack-Based Interpreter & VM                    //
//   / / / / / /  > · <      C++23 · Single-Header Library                    //
//  /_/ /_/ /_/  /_/\_\     Copyright 2026 Mark Guidarelli                    //
//                                                                            //
// Licensed under the Apache License, Version 2.0 (the "License");            //
// you may not use this file except in compliance with the License.           //
// You may obtain a copy of the License at                                    //
//                                                                            //
//     https://www.apache.org/licenses/LICENSE-2.0                            //
//                                                                            //
// Unless required by applicable law or agreed to in writing, software        //
// distributed under the License is distributed on an "AS IS" BASIS,          //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.   //
// See the License for the specific language governing permissions and        //
// limitations under the License.                                             //
//                                                                            //
//===----------------------------------------------------------------------===//
private:
//===--- Scanner Character Attribute Tables and Binary Token Maps ---===//
using chattr_t = uint16_t;
static constexpr chattr_t WS{0x0001};  //            whitespace: HT LF VT FF CR SP
static constexpr chattr_t CT{0x0002};  //    comment terminator: LF FF CR
static constexpr chattr_t DL{0x0004};  //             delimiter: % ( ) / \ . < > [ ] { }
static constexpr chattr_t RG{0x0008};  //               regular: !..~ except DL
static constexpr chattr_t LC{0x0010};  //            lower case: a..z
static constexpr chattr_t UC{0x0020};  //            upper case: A..Z
static constexpr chattr_t DD{0x0040};  //         decimal digit: 0..9
static constexpr chattr_t OD{0x0080};  //           octal digit: 0..7
static constexpr chattr_t HD{0x0100};  //             hex digit: 0..9 A..F a..f
static constexpr chattr_t IN{0x0200};  //               integer: + - 0..9
static constexpr chattr_t FE{0x0400};  //           frac or exp: . E e
static constexpr chattr_t IS{0x0800};  //       integral suffix: A B I L Q U a b i l q u
static constexpr chattr_t FS{0x1000};  // floating point suffix: D E R d e r
static constexpr chattr_t SE{0x2000};  //          syntax-error: (NUL..US except HT LF VT FF CR)) DEL
static constexpr chattr_t US{0x4000};  //            underscore: _
static constexpr chattr_t SS{0x8000};  //           string-stop: ( ) \ CR  (fast-path exit for (string)/<(raw string)>)
static constexpr chattr_t BT{0x0000};  //           binarytoken: 80..FF

static_assert('a' > 'A', "Trix assumes the source charset's 'a' sorts after 'A' (ASCII / UTF-8).");

// sm_chdata has 257 entries: index 0 holds EOF(-1) attributes; sm_chattr = &sm_chdata[1]
// so that sm_chattr[-1] == sm_chdata[0] legally handles EOFc without bounds checks.
// All predicates (is_whitespace, is_delimiter, etc.) accept the range [-1..255].
static constexpr chattr_t sm_chdata[1 + 256] = {
        0,                         // -1     EOF End of File
        SE,                        // 00  ^@ NUL Null
        SE,                        // 01  ^A SOH Start of Heading
        SE,                        // 02  ^B STX Start of Text
        SE,                        // 03  ^C ETX End of Text
        SE,                        // 04  ^D EOT End of Transmission
        SE,                        // 05  ^E ENQ Enquiry
        SE,                        // 06  ^F ACK Acknowledgement
        SE,                        // 07  ^G BEL Bell
        SE,                        // 08  ^H BS  Backspace
        WS,                        // 09  ^I HT  Horizontal Tab
        (WS | CT),                 // 0A  ^J LF  Line Feed
        WS,                        // 0B  ^K VT  Vertical Tab
        (WS | CT),                 // 0C  ^L FF  Form Feed
        (WS | CT | SS),            // 0D  ^M CR  Carriage Return
        SE,                        // 0E  ^N SO  Shift Out
        SE,                        // 0F  ^O SI  Shift In
        SE,                        // 10  ^P DLE Data Link Escape
        SE,                        // 11  ^Q DC1 Device Control 1/XON
        SE,                        // 12  ^R DC2 Device Control 2
        SE,                        // 13  ^S DC3 Device Control 3/XOFF
        SE,                        // 14  ^T DC4 Device Control 4
        SE,                        // 15  ^U NAK Negative Acknowledgement
        SE,                        // 16  ^V SYN Synchronous Idle
        SE,                        // 17  ^W ETB End of Transmission Block
        SE,                        // 18  ^X CAN Cancel
        SE,                        // 19  ^Y EM  End of Medium
        SE,                        // 1A  ^Z SUB Substitute
        SE,                        // 1B  ^[ ESC Escape
        SE,                        // 1C  ^\ FS  File Separator
        SE,                        // 1D  ^] GS  Group Separator
        SE,                        // 1E  ^^ RS  Record Separator
        SE,                        // 1F  ^_ US  Unit Separator
        WS,                        // 20 " " SP  Space
        RG,                        // 21 "!"     Base-64 --string--
        RG,                        // 22 """
        RG,                        // 23 "#"     radix number, suffix initiator
        RG,                        // 24 "$"
        DL,                        // 25 "%"     comment initiator
        RG,                        // 26 "&"
        RG,                        // 27 "'"
        (DL | SS),                 // 28 "("     start --string--
        (DL | SS),                 // 29 ")"     end --string--
        RG,                        // 2A "*"
        (RG | IN),                 // 2B "+"     positive number
        RG,                        // 2C ","
        (RG | IN),                 // 2D "-"     negative number
        (DL | FE),                 // 2E "."     field-access prefix (.name -> /name get); FE for number-state interior
        DL,                        // 2F "/"     literal --name--
        (RG | DD | HD | IN | OD),  // 30 "0"
        (RG | DD | HD | IN | OD),  // 31 "1"
        (RG | DD | HD | IN | OD),  // 32 "2"
        (RG | DD | HD | IN | OD),  // 33 "3"
        (RG | DD | HD | IN | OD),  // 34 "4"
        (RG | DD | HD | IN | OD),  // 35 "5"
        (RG | DD | HD | IN | OD),  // 36 "6"
        (RG | DD | HD | IN | OD),  // 37 "7"
        (RG | DD | HD | IN),       // 38 "8"
        (RG | DD | HD | IN),       // 39 "9"
        (RG),                      // 3A ":"     directory path separator
        (RG),                      // 3B ";"
        DL,                        // 3C "<"     start hex --string--, --dict--
        RG,                        // 3D "="     =string, =array, =dict, =proc,  suffix
        DL,                        // 3E ">"     end hex --string--, --dict--
        RG,                        // 3F "?"
        RG,                        // 40 "@"     name prefix for hidden system --operator--
        (RG | UC | HD | IS),       // 41 "A"
        (RG | UC | HD | IS),       // 42 "B"
        (RG | UC | HD),            // 43 "C"
        (RG | UC | HD | FS),       // 44 "D"
        (RG | UC | HD | FE | FS),  // 45 "E"
        (RG | UC | HD),            // 46 "F"
        (RG | UC),                 // 47 "G"
        (RG | UC),                 // 48 "H"
        (RG | UC | IS),            // 49 "I"
        (RG | UC),                 // 4A "J"
        (RG | UC),                 // 4B "K"
        (RG | UC | IS),            // 4C "L"
        (RG | UC),                 // 4D "M"
        (RG | UC),                 // 4E "N"
        (RG | UC),                 // 4F "O"
        (RG | UC),                 // 50 "P"
        (RG | UC | IS),            // 51 "Q"     --int128-- suffix (quad)
        (RG | UC | FS),            // 52 "R"
        (RG | UC),                 // 53 "S"
        (RG | UC),                 // 54 "T"
        (RG | UC | IS),            // 55 "U"
        (RG | UC),                 // 56 "V"
        (RG | UC),                 // 57 "W"
        (RG | UC),                 // 58 "X"
        (RG | UC),                 // 59 "Y"
        (RG | UC),                 // 5A "Z"
        DL,                        // 5B "["     start --array--
        (DL | SS),                 // 5C "\"     executable --name--
        DL,                        // 5D "]"     end --array--
        RG,                        // 5E "^"
        (RG | US),                 // 5F "_"     underscore in number
        RG,                        // 60 "`"
        (RG | LC | HD | IS),       // 61 "a"     --address--, --array-- suffix
        (RG | LC | HD | IS),       // 62 "b"     --byte-- suffix
        (RG | LC | HD),            // 63 "c"
        (RG | LC | HD | FS),       // 64 "d"     --double-- suffix
        (RG | LC | HD | FE | FS),  // 65 "e"     exponent, early binding suffix
        (RG | LC | HD),            // 66 "f"
        (RG | LC),                 // 67 "g"
        (RG | LC),                 // 68 "h"
        (RG | LC | IS),            // 69 "i"     --integer-- suffix
        (RG | LC),                 // 6A "j"
        (RG | LC),                 // 6B "k"
        (RG | LC | IS),            // 6C "l"     --long-- suffix, literal suffix, late binding suffix
        (RG | LC),                 // 6D "m"
        (RG | LC),                 // 6E "n"
        (RG | LC),                 // 6F "o"
        (RG | LC),                 // 70 "p"     --packed-- suffix
        (RG | LC | IS),            // 71 "q"     --int128-- suffix (quad)
        (RG | LC | FS),            // 72 "r"     --real-- suffix, readonly suffix
        (RG | LC),                 // 73 "s"
        (RG | LC),                 // 74 "t"
        (RG | LC | IS),            // 75 "u"     unsigned suffix
        (RG | LC),                 // 76 "v"
        (RG | LC),                 // 77 "w"     writable suffix
        (RG | LC),                 // 78 "x"     executable suffix
        (RG | LC),                 // 79 "y"
        (RG | LC),                 // 7A "z"
        DL,                        // 7B "{"     start --proc--
        RG,                        // 7C "|"     local variable binding delimiter
        DL,                        // 7D "}"     end --proc--
        RG,                        // 7E "~"
        SE,                        // 7F  ^? DEL Delete
        BT,                        // 80         BinaryToken::Byte
        BT,                        // 81         BinaryToken::Integer_8
        BT,                        // 82         BinaryToken::Integer_16
        BT,                        // 83         BinaryToken::Integer_16b
        BT,                        // 84         BinaryToken::Integer_16l
        BT,                        // 85         BinaryToken::Integer_32
        BT,                        // 86         BinaryToken::Integer_32b
        BT,                        // 87         BinaryToken::Integer_32l
        BT,                        // 88         BinaryToken::UInteger_8
        BT,                        // 89         BinaryToken::UInteger_16
        BT,                        // 8A         BinaryToken::UInteger_16b
        BT,                        // 8B         BinaryToken::UInteger_16l
        BT,                        // 8C         BinaryToken::UInteger_32
        BT,                        // 8D         BinaryToken::UInteger_32b
        BT,                        // 8E         BinaryToken::UInteger_32l
        BT,                        // 8F         BinaryToken::Long_8
        BT,                        // 90         BinaryToken::Long_16
        BT,                        // 91         BinaryToken::Long_16b
        BT,                        // 92         BinaryToken::Long_16l
        BT,                        // 93         BinaryToken::Long_32
        BT,                        // 94         BinaryToken::Long_32b
        BT,                        // 95         BinaryToken::Long_32l
        BT,                        // 96         BinaryToken::Long_64
        BT,                        // 97         BinaryToken::Long_64b
        BT,                        // 98         BinaryToken::Long_64l
        BT,                        // 99         BinaryToken::ULong_8
        BT,                        // 9A         BinaryToken::ULong_16
        BT,                        // 9B         BinaryToken::ULong_16b
        BT,                        // 9C         BinaryToken::ULong_16l
        BT,                        // 9D         BinaryToken::ULong_32
        BT,                        // 9E         BinaryToken::ULong_32b
        BT,                        // 9F         BinaryToken::ULong_32l
        BT,                        // A0         BinaryToken::ULong_64
        BT,                        // A1         BinaryToken::ULong_64b
        BT,                        // A2         BinaryToken::ULong_64l
        BT,                        // A3         BinaryToken::Real
        BT,                        // A4         BinaryToken::Real_b
        BT,                        // A5         BinaryToken::Real_l
        BT,                        // A6         BinaryToken::Double
        BT,                        // A7         BinaryToken::Double_b
        BT,                        // A8         BinaryToken::Double_l
        BT,                        // A9         BinaryToken::Fixed
        BT,                        // AA         BinaryToken::False
        BT,                        // AB         BinaryToken::True
        BT,                        // AC         BinaryToken::Mark
        BT,                        // AD         BinaryToken::Null
        BT,                        // AE         BinaryToken::String_8
        BT,                        // AF         BinaryToken::String_16
        BT,                        // B0         BinaryToken::String_16b
        BT,                        // B1         BinaryToken::String_16l
        BT,                        // B2         BinaryToken::SystemLitName_8
        BT,                        // B3         BinaryToken::SystemLitName_16
        BT,                        // B4         BinaryToken::SystemLitName_16b
        BT,                        // B5         BinaryToken::SystemLitName_16l
        BT,                        // B6         BinaryToken::SystemExecName_8
        BT,                        // B7         BinaryToken::SystemExecName_16
        BT,                        // B8         BinaryToken::SystemExecName_16b
        BT,                        // B9         BinaryToken::SystemExecName_16l
        BT,                        // BA         BinaryToken::SystemLitValue_8
        BT,                        // BB         BinaryToken::SystemLitValue_16
        BT,                        // BC         BinaryToken::SystemLitValue_16b
        BT,                        // BD         BinaryToken::SystemLitValue_16l
        BT,                        // BE         BinaryToken::SystemExecValue_8
        BT,                        // BF         BinaryToken::SystemExecValue_16
        BT,                        // C0         BinaryToken::SystemExecValue_16b
        BT,                        // C1         BinaryToken::SystemExecValue_16l
        BT,                        // C2         BinaryToken::Integer_2
        BT,                        // C3         BinaryToken::Integer_10
        BT,                        // C4         BinaryToken::Integer_100
        BT,                        // C5         BinaryToken::Byte_2
        BT,                        // C6         BinaryToken::NumberArray
        BT,                        // C7         BinaryToken::Byte_0
        BT,                        // C8         BinaryToken::Byte_1
        BT,                        // C9         BinaryToken::Byte_127
        BT,                        // CA         BinaryToken::Byte_128
        BT,                        // CB         BinaryToken::Byte_255
        BT,                        // CC         BinaryToken::Integer_Min
        BT,                        // CD         BinaryToken::Integer_Max
        BT,                        // CE         BinaryToken::Integer_Neg1
        BT,                        // CF         BinaryToken::Integer_0
        BT,                        // D0         BinaryToken::Integer_1
        BT,                        // D1         BinaryToken::UInteger_Min
        BT,                        // D2         BinaryToken::UInteger_Max
        BT,                        // D3         BinaryToken::UInteger_1
        BT,                        // D4         BinaryToken::Long_Min
        BT,                        // D5         BinaryToken::Long_Max
        BT,                        // D6         BinaryToken::Long_Neg1
        BT,                        // D7         BinaryToken::Long_0
        BT,                        // D8         BinaryToken::Long_1
        BT,                        // D9         BinaryToken::ULong_Min
        BT,                        // DA         BinaryToken::ULong_Max
        BT,                        // DB         BinaryToken::ULong_1
        BT,                        // DC         BinaryToken::Real_Neg1
        BT,                        // DD         BinaryToken::Real_0
        BT,                        // DE         BinaryToken::Real_1
        BT,                        // DF         BinaryToken::Real_e
        BT,                        // E0         BinaryToken::Real_pi
        BT,                        // E1         BinaryToken::Real_INF
        BT,                        // E2         BinaryToken::Real_qNAN
        BT,                        // E3         BinaryToken::Double_Neg1
        BT,                        // E4         BinaryToken::Double_0
        BT,                        // E5         BinaryToken::Double_1
        BT,                        // E6         BinaryToken::Double_e
        BT,                        // E7         BinaryToken::Double_pi
        BT,                        // E8         BinaryToken::Double_INF
        BT,                        // E9         BinaryToken::Double_qNAN
        BT,                        // EA         BinaryToken::Address_nullptr
        BT,                        // EB         BinaryToken::Integer_Neg2
        BT,                        // EC         BinaryToken::Real_2
        BT,                        // ED         BinaryToken::Double_2
        BT,                        // EE         BinaryToken::WellKnownLitName
        BT,                        // EF         BinaryToken::WellKnownExecName
        BT,                        // F0         BinaryToken::Reserved_F0
        BT,                        // F1         BinaryToken::Reserved_F1
        BT,                        // F2         BinaryToken::Reserved_F2
        BT,                        // F3         BinaryToken::Reserved_F3
        BT,                        // F4         BinaryToken::Reserved_F4
        BT,                        // F5         BinaryToken::Reserved_F5
        BT,                        // F6         BinaryToken::Reserved_F6
        BT,                        // F7         BinaryToken::Reserved_F7
        BT,                        // F8         BinaryToken::Reserved_F8
        BT,                        // F9         BinaryToken::Reserved_F9
        BT,                        // FA         BinaryToken::Reserved_FA
        BT,                        // FB         BinaryToken::Reserved_FB
        BT,                        // FC         BinaryToken::Reserved_FC
        BT,                        // FD         BinaryToken::Reserved_FD
        BT,                        // FE         BinaryToken::Reserved_FE
        BT                         // FF         BinaryToken::Reserved_FF
};
static constexpr const chattr_t *sm_chattr = &sm_chdata[1];

// non printable ASCII characters
static constexpr vm_t ASCII_NUL{0x00};  // ^@ Null
static constexpr vm_t ASCII_SOH{0x01};  // ^A Start of Heading
static constexpr vm_t ASCII_STX{0x02};  // ^B Start of Text
static constexpr vm_t ASCII_ETX{0x03};  // ^C End of Text
static constexpr vm_t ASCII_EOT{0x04};  // ^D End of Transmission
static constexpr vm_t ASCII_ENQ{0x05};  // ^E Enquiry
static constexpr vm_t ASCII_ACK{0x06};  // ^F Acknowledgement
static constexpr vm_t ASCII_BEL{0x07};  // ^G Bell            \a
static constexpr vm_t ASCII_BS{0x08};   // ^H Backspace       \b
static constexpr vm_t ASCII_HT{0x09};   // ^I Horizontal Tab  \t
static constexpr vm_t ASCII_LF{0x0A};   // ^J Line Feed       \n
static constexpr vm_t ASCII_VT{0x0B};   // ^K Vertical Tab    \v
static constexpr vm_t ASCII_FF{0x0C};   // ^L Form Feed       \f
static constexpr vm_t ASCII_CR{0x0D};   // ^M Carriage Return \r
static constexpr vm_t ASCII_SO{0x0E};   // ^N Shift Out
static constexpr vm_t ASCII_SI{0x0F};   // ^O Shift In
static constexpr vm_t ASCII_DLE{0x10};  // ^P Data Link Escape
static constexpr vm_t ASCII_DC1{0x11};  // ^Q Device Control 1/XON
static constexpr vm_t ASCII_DC2{0x12};  // ^R Device Control 2
static constexpr vm_t ASCII_DC3{0x13};  // ^S Device Control 3/XOFF
static constexpr vm_t ASCII_DC4{0x14};  // ^T Device Control 4
static constexpr vm_t ASCII_NAK{0x15};  // ^U Negative Acknowledgement
static constexpr vm_t ASCII_SYN{0x16};  // ^V Synchronous Idle
static constexpr vm_t ASCII_ETB{0x17};  // ^W End of Transmission Block
static constexpr vm_t ASCII_CAN{0x18};  // ^X Cancel
static constexpr vm_t ASCII_EM{0x19};   // ^Y End of Medium
static constexpr vm_t ASCII_SUB{0x1A};  // ^Z Substitute
static constexpr vm_t ASCII_ESC{0x1B};  // ^[ Escape          \e
static constexpr vm_t ASCII_FS{0x1C};   // ^\ File Separator
static constexpr vm_t ASCII_GS{0x1D};   // ^] Group Separator
static constexpr vm_t ASCII_RS{0x1E};   // ^^ Record Separator
static constexpr vm_t ASCII_US{0x1F};   // ^_ Unit Separator
static constexpr vm_t ASCII_SP{0x20};   //    Space
static constexpr vm_t ASCII_DEL{0x7F};  // ^? Delete

[[nodiscard]] static constexpr bool is_comment_terminator(int ch) {
    assert((ch >= -1) && (ch <= 255));

    return ((sm_chattr[ch] & CT) != 0);
}

[[nodiscard]] static constexpr bool is_whitespace(int ch) {
    assert((ch >= -1) && (ch <= 255));

    return ((sm_chattr[ch] & WS) != 0);
}

[[nodiscard]] static constexpr bool is_delimiter(int ch) {
    assert((ch >= -1) && (ch <= 255));

    return ((sm_chattr[ch] & DL) != 0);
}

[[nodiscard]] static constexpr bool is_regular(int ch) {
    assert((ch >= -1) && (ch <= 255));

    return ((sm_chattr[ch] & RG) != 0);
}

// whitespace, delimiter, binary-token, syntax-error
[[nodiscard]] static constexpr bool is_terminator(int ch) {
    assert((ch >= -1) && (ch <= 255));

    return !is_regular(ch);
}

[[nodiscard]] static constexpr bool is_lowercase(int ch) {
    assert((ch >= -1) && (ch <= 255));

    return ((sm_chattr[ch] & LC) != 0);
}

[[nodiscard]] static constexpr bool is_uppercase(int ch) {
    assert((ch >= -1) && (ch <= 255));

    return ((sm_chattr[ch] & UC) != 0);
}

[[nodiscard]] static constexpr bool is_alpha(int ch) {
    assert((ch >= -1) && (ch <= 255));

    return ((sm_chattr[ch] & (LC | UC)) != 0);
}

[[nodiscard]] static constexpr bool is_alpha_or_underscore(int ch) {
    assert((ch >= -1) && (ch <= 255));

    return ((sm_chattr[ch] & (LC | UC | US)) != 0);
}

[[nodiscard]] static constexpr int to_lowercase(int ch) {
    assert((ch >= -1) && (ch <= 255));

    return (((sm_chattr[ch] & UC) != 0) ? (ch + ('a' - 'A')) : ch);
}

[[nodiscard]] static constexpr vm_t to_lowercase(vm_t ch) {
    return (((sm_chattr[ch] & UC) != 0) ? static_cast<vm_t>(ch + ('a' - 'A')) : ch);
}

[[nodiscard]] static constexpr int to_uppercase(int ch) {
    assert((ch >= -1) && (ch <= 255));

    return (((sm_chattr[ch] & LC) != 0) ? (ch - ('a' - 'A')) : ch);
}

[[nodiscard]] static constexpr vm_t to_uppercase(vm_t ch) {
    return (((sm_chattr[ch] & LC) != 0) ? static_cast<vm_t>(ch - ('a' - 'A')) : ch);
}

[[nodiscard]] static constexpr bool is_digit(int ch) {
    assert((ch >= -1) && (ch <= 255));

    return ((sm_chattr[ch] & DD) != 0);
}

[[nodiscard]] static constexpr bool is_digit_or_alpha(int ch) {
    assert((ch >= -1) && (ch <= 255));

    return ((sm_chattr[ch] & (DD | LC | UC)) != 0);
}

[[nodiscard]] static constexpr bool is_digit_or_alpha_or_underscore(int ch) {
    assert((ch >= -1) && (ch <= 255));

    return ((sm_chattr[ch] & (DD | LC | UC | US)) != 0);
}

[[nodiscard]] static constexpr bool is_octaldigit(int ch) {
    assert((ch >= -1) && (ch <= 255));

    return ((sm_chattr[ch] & OD) != 0);
}

[[nodiscard]] static std::optional<vm_t> unescape_letter(vm_t ch) {
    switch (ch) {
    case 'a':
        return ASCII_BEL;

    case 'b':
        return ASCII_BS;

    case 'e':
        return ASCII_ESC;

    case 'f':
        return ASCII_FF;

    case 'n':
        return ASCII_LF;

    case 'r':
        return ASCII_CR;

    case 't':
        return ASCII_HT;

    case 'v':
        return ASCII_VT;

    default:
        return std::nullopt;
    }
}

// Returns true if ch can appear in a numeric token: digits, letters
// (radix digits and type suffixes), sign, decimal point, radix marker.
[[nodiscard]] static constexpr bool is_numeric(int ch) {
    assert((ch >= -1) && (ch <= 255));

    return (((sm_chattr[ch] & (LC | UC | IN | FE)) != 0) || (ch == '#'));
}

[[nodiscard]] static constexpr bool is_hexdigit(int ch) {
    assert((ch >= -1) && (ch <= 255));

    return ((sm_chattr[ch] & HD) != 0);
}

[[nodiscard]] static constexpr bool is_integer(int ch) {
    assert((ch >= -1) && (ch <= 255));

    return ((sm_chattr[ch] & IN) != 0);
}

[[nodiscard]] static constexpr bool is_real(int ch) {
    assert((ch >= -1) && (ch <= 255));

    return ((sm_chattr[ch] & (IN | FE)) != 0);
}

[[nodiscard]] static constexpr bool is_frac_or_exp(int ch) {
    assert((ch >= -1) && (ch <= 255));

    return ((sm_chattr[ch] & FE) != 0);
}

[[nodiscard]] static constexpr bool is_integral_suffix(int ch) {
    assert((ch >= -1) && (ch <= 255));

    return ((sm_chattr[ch] & IS) != 0);
}

[[nodiscard]] static constexpr bool is_floatingpoint_suffix(int ch) {
    assert((ch >= -1) && (ch <= 255));

    return ((sm_chattr[ch] & FS) != 0);
}

[[nodiscard]] static constexpr bool is_binary_token(int ch) {
    assert((ch >= -1) && (ch <= 255));

    return (ch >= 0x80);
}

[[nodiscard]] static constexpr bool is_syntax_error(int ch) {
    assert((ch >= -1) && (ch <= 255));

    return ((sm_chattr[ch] & SE) != 0);
}

// Fast-path exit for (string) and <(raw string)> scanners: '(' ')' '\\' ASCII_CR.
[[nodiscard]] static constexpr bool is_string_stop(int ch) {
    assert((ch >= -1) && (ch <= 255));

    return ((sm_chattr[ch] & SS) != 0);
}

[[nodiscard]] static constexpr int hexdigit_to_value(int ch) {
    assert(Trix::is_hexdigit(ch));

    if (Trix::is_digit(ch)) {
        return (ch - '0');
    } else if (Trix::is_uppercase(ch)) {
        return (ch - 'A' + 10);
    } else {
        return (ch - 'a' + 10);
    }
}

//===--- Compile-time character-class verification ---===//
//
// The expected-character lists below are the SPECIFICATION for the sm_chattr
// bit table; verify_chattr() cross-checks every predicate against its list
// over the full 0..255 range.  This ran for years as a debug-startup assert
// (init.inl); since sm_chattr and all is_* predicates are constexpr, the
// whole check now runs in the COMPILER: trix.h static_asserts
// verify_chattr() right after class Trix closes, so any drift between the
// bit table and the lists fails every build (release included) instead of
// the first debug run.
//
// Debugging a failure: the static_assert in trix.h names this function; the
// per-table sub-checks below are ordered, so temporarily replace the single
// static_assert with per-table 'static_assert(check_chattr(...) == 0)'
// probes (the lists are local statics here) to bisect, or just diff the
// touched list against the sm_chdata row edits that triggered the failure.
public:
// Count mismatches between a predicate and its expected-character list over
// the full byte range.  consteval: callable only during constant evaluation.
[[nodiscard]] static consteval int check_chattr(std::span<const vm_t> chars, bool (*f)(int ch)) {
    bool valid[256] = {};

    for (auto ch : chars) {
        valid[ch] = true;
    }

    auto count = 0;
    for (int i = 0; i < 256; ++i) {
        if (f(i) ^ valid[i]) {
            ++count;
        }
    }
    return count;
}

// Validate every character-class predicate against its specification list.
// Evaluated by the static_assert at the bottom of trix.h.
[[nodiscard]] static consteval bool verify_chattr() {
    auto err_count = 0;

    static constexpr vm_t whitespace[] = {ASCII_HT, ASCII_LF, ASCII_VT, ASCII_FF, ASCII_CR, ASCII_SP};
    err_count += check_chattr(whitespace, is_whitespace);

    static constexpr vm_t comment_terminator[] = {ASCII_LF, ASCII_FF, ASCII_CR};
    err_count += check_chattr(comment_terminator, is_comment_terminator);

    static constexpr vm_t delimiter[] = {'%', '(', ')', '/', '\\', '.', '<', '>', '[', ']', '{', '}'};
    err_count += check_chattr(delimiter, is_delimiter);

    static constexpr vm_t regular[] = {'!', '"', '#', '$', '&', '\'', '*', '+', ',', '-', '0', '1', '2', '3', '4', '5', '6',
                                       '7', '8', '9', ':', ';', '=',  '?', '@', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I',
                                       'J', 'K', 'L', 'M', 'N', 'O',  'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
                                       '^', '_', '`', 'a', 'b', 'c',  'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n',
                                       'o', 'p', 'q', 'r', 's', 't',  'u', 'v', 'w', 'x', 'y', 'z', '|', '~'};
    err_count += check_chattr(regular, is_regular);

    static constexpr vm_t lowercase[] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
                                         'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z'};
    err_count += check_chattr(lowercase, is_lowercase);

    static constexpr vm_t uppercase[] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
                                         'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z'};
    err_count += check_chattr(uppercase, is_uppercase);

    static constexpr vm_t digit[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};
    err_count += check_chattr(digit, is_digit);

    static constexpr vm_t octaldigit[] = {'0', '1', '2', '3', '4', '5', '6', '7'};
    err_count += check_chattr(octaldigit, is_octaldigit);

    static constexpr vm_t hexdigit[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A',
                                        'B', 'C', 'D', 'E', 'F', 'a', 'b', 'c', 'd', 'e', 'f'};
    err_count += check_chattr(hexdigit, is_hexdigit);

    static constexpr vm_t integer[] = {'+', '-', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};
    err_count += check_chattr(integer, is_integer);

    static constexpr vm_t real[] = {'+', '-', '.', 'E', 'e', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};
    err_count += check_chattr(real, is_real);

    static constexpr vm_t frac_or_exp[] = {'.', 'E', 'e'};
    err_count += check_chattr(frac_or_exp, is_frac_or_exp);

    static constexpr vm_t integral_suffix[] = {'A', 'B', 'I', 'L', 'Q', 'U', 'a', 'b', 'i', 'l', 'q', 'u'};
    err_count += check_chattr(integral_suffix, is_integral_suffix);

    static constexpr vm_t floatingpoint_suffix[] = {'D', 'E', 'R', 'd', 'e', 'r'};
    err_count += check_chattr(floatingpoint_suffix, is_floatingpoint_suffix);

    static constexpr vm_t syntax_error[] = {ASCII_NUL, ASCII_SOH, ASCII_STX, ASCII_ETX, ASCII_EOT, ASCII_ENQ, ASCII_ACK,
                                            ASCII_BEL, ASCII_BS,  ASCII_SO,  ASCII_SI,  ASCII_DLE, ASCII_DC1, ASCII_DC2,
                                            ASCII_DC3, ASCII_DC4, ASCII_NAK, ASCII_SYN, ASCII_ETB, ASCII_CAN, ASCII_EM,
                                            ASCII_SUB, ASCII_ESC, ASCII_FS,  ASCII_GS,  ASCII_RS,  ASCII_US,  ASCII_DEL};
    err_count += check_chattr(syntax_error, is_syntax_error);

    static constexpr vm_t binary_token[] = {
            0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F, 0x90, 0x91, 0x92,
            0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5,
            0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8,
            0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB,
            0xCC, 0xCD, 0xCE, 0xCF, 0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE,
            0xDF, 0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF, 0xF0, 0xF1,
            0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF};
    err_count += check_chattr(binary_token, is_binary_token);

    // Invariant: every comment-terminator character must also be whitespace.
    // scanner() relies on this: after consuming a comment the terminator is retested
    // by is_whitespace().  A CT-only character would escape the outer whitespace-skip
    // loop and be dispatched as a regular token instead of being consumed as whitespace.
    for (int i = 0; i < 256; ++i) {
        if (is_comment_terminator(i) && !is_whitespace(i)) {
            ++err_count;
        }
    }

    return (err_count == 0);
}
private:
