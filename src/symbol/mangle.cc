// Copyright 2026 The Alcy Project Authors
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "symbol/mangle.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ir/common.h"
#include "ir/storage.h"

namespace symbol {

namespace {

// The encoded form. Every symbol starts with `_A` and a format version,
// so a decoder rejects what it does not understand instead of guessing.
//
//   symbol   = "_A" version kind seglist name generics
//   kind     = "f" | "a" | "m"
//   seglist  = { segment } "."
//   segment  = DIGITS name            DIGITS is the byte length of name
//   name     = DIGITS name
//   generics = { type }
//   type     = prim | "z" | "p"
//            | "r" type | "w" type
//            | "y" DIGITS type
//            | "u" DIGITS { type }
//            | "n" seglist DIGITS { type }
//   prim     = one character from the table below
//
// A list of segments ends at a ".", because the item that follows a
// segment list is itself length-prefixed and would otherwise be read as
// one more segment. A list of types ends where a type tag or the end of
// the string appears, since a tag is a letter and a length is a digit.
// A tuple and a nominal's argument list carry an explicit count, so
// their extent does not depend on what follows.
constexpr std::string_view kPrefix = "_A";
constexpr std::string_view kVersion = "1";
// Ends a list of length-prefixed segments.
constexpr char kListEnd = '.';

// Primitives, one character each. The letters are chosen to be
// memorable; `display` turns them back into source spelling, which is
// why the encoding itself does not have to be readable.
constexpr char kTagBool = 'b';
constexpr char kTagI8 = 'h';
constexpr char kTagI16 = 'H';
constexpr char kTagI32 = 'i';
constexpr char kTagI64 = 'I';
constexpr char kTagU8 = 't';
constexpr char kTagU16 = 'T';
constexpr char kTagU32 = 'U';
constexpr char kTagU64 = 'L';
constexpr char kTagF32 = 'f';
constexpr char kTagF64 = 'F';
constexpr char kTagStr = 'z';
constexpr char kTagPtr = 'p';
constexpr char kTagRef = 'r';
constexpr char kTagMutRef = 'w';
constexpr char kTagArray = 'y';
constexpr char kTagTuple = 'u';
constexpr char kTagNominal = 'n';

struct Primitive {
  char tag;
  std::string_view spelling;
};

constexpr Primitive kPrimitives[] = {
    {kTagBool, "bool"}, {kTagI8, "i8"},   {kTagI16, "i16"}, {kTagI32, "i32"},
    {kTagI64, "i64"},   {kTagU8, "u8"},   {kTagU16, "u16"}, {kTagU32, "u32"},
    {kTagU64, "u64"},   {kTagF32, "f32"}, {kTagF64, "f64"},
};

std::string_view primitive_spelling(char tag) {
  for (const Primitive& primitive : kPrimitives) {
    if (primitive.tag == tag) {
      return primitive.spelling;
    }
  }
  return {};
}

char primitive_tag(ir::TypeTag tag) {
  switch (tag) {
    case ir::TypeTag::I1: return kTagBool;
    case ir::TypeTag::I8: return kTagI8;
    case ir::TypeTag::I16: return kTagI16;
    case ir::TypeTag::I32: return kTagI32;
    case ir::TypeTag::I64: return kTagI64;
    case ir::TypeTag::U8: return kTagU8;
    case ir::TypeTag::U16: return kTagU16;
    case ir::TypeTag::U32: return kTagU32;
    case ir::TypeTag::U64: return kTagU64;
    case ir::TypeTag::F32: return kTagF32;
    case ir::TypeTag::F64: return kTagF64;
    default: return '\0';
  }
}

char kind_tag(Signature::Kind kind) {
  switch (kind) {
    case Signature::Kind::Free: return 'f';
    case Signature::Kind::Assoc: return 'a';
    case Signature::Kind::Method: return 'm';
    case Signature::Kind::Foreign: return '\0';
  }
  return '\0';
}

void append_u64(std::string& out, u64 value) {
  char digits[20];
  u32 count = 0;
  do {
    digits[count++] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value != 0);
  while (count > 0) {
    out.push_back(digits[--count]);
  }
}

// A module path arrives as one `::`-separated string, matching how the
// module tree spells it. Encoding it segment by segment with explicit
// lengths means a segment may contain any byte, separators included.
void append_path(std::string& out, std::string_view path) {
  constexpr std::string_view kSeparator = "::";
  usize start = 0;
  while (start <= path.size()) {
    const usize split = path.find(kSeparator, start);
    const std::string_view segment = split == std::string_view::npos
                                         ? path.substr(start)
                                         : path.substr(start, split - start);
    if (!segment.empty()) {
      append_u64(out, segment.size());
      out.append(segment);
    }
    if (split == std::string_view::npos) {
      break;
    }
    start = split + kSeparator.size();
  }
  out.push_back(kListEnd);
}

class Encoder {
 public:
  Encoder(const ir::Storage& types,
          const str::StringInterner& strings,
          std::string& out)
      : types_(types), strings_(strings), out_(out) {}

  void encode_type(ir::TypeIdx type) {
    const ir::TypeNode& node = types_.types()[type.idx];
    if (const char tag = primitive_tag(node.tag); tag != '\0') {
      out_.push_back(tag);
      return;
    }
    switch (node.tag) {
      case ir::TypeTag::Str: out_.push_back(kTagStr); return;
      case ir::TypeTag::Ref:
        out_.push_back(kTagRef);
        encode_type(pointee(node));
        return;
      case ir::TypeTag::MutRef:
        out_.push_back(kTagMutRef);
        encode_type(pointee(node));
        return;
      case ir::TypeTag::Ptr: out_.push_back(kTagPtr); return;
      case ir::TypeTag::Array: {
        const ir::ArrayType& array = types_.array_types()[node.as_array()];
        out_.push_back(kTagArray);
        append_u64(out_, array.count);
        encode_type(array.element);
        return;
      }
      case ir::TypeTag::Tuple: {
        const ir::TypeIdxRange elements =
            types_.tuple_types()[node.as_tuple()].elements;
        out_.push_back(kTagTuple);
        append_u64(out_, elements.size());
        for (const ir::TypeIdx element : elements) {
          encode_type(element);
        }
        return;
      }
      case ir::TypeTag::Struct:
      case ir::TypeTag::Enum: encode_nominal(node); return;
      default:
        // Void, Never, Error, and Function never appear in a signature
        // the linker sees. Encoding one as `str` keeps the mapping
        // total; nothing decodes a symbol back into a program.
        out_.push_back(kTagStr);
        return;
    }
  }

 private:
  void encode_nominal(const ir::TypeNode& node) {
    out_.push_back(kTagNominal);
    std::string_view name;
    ir::TypeIdxRange params;
    if (node.tag == ir::TypeTag::Struct) {
      const ir::StructType& shape = types_.struct_types()[node.as_struct()];
      name = strings_.get(shape.name);
      params = shape.params;
    } else {
      const ir::EnumType& shape = types_.enum_types()[node.as_enum()];
      name = strings_.get(shape.name);
      params = shape.params;
    }
    append_path(out_, name);
    append_u64(out_, params.size());
    for (const ir::TypeIdx param : params) {
      encode_type(param);
    }
  }

  ir::TypeIdx pointee(const ir::TypeNode& node) const {
    return types_.ref_types()[node.as_ref()].pointee;
  }

  const ir::Storage& types_;
  const str::StringInterner& strings_;
  std::string& out_;
};

// A cursor over an encoded symbol. Every read either advances past a
// well-formed item or reports failure, so a truncated or foreign symbol
// cannot be mistaken for a valid one.
class Decoder {
 public:
  explicit Decoder(std::string_view text) : text_(text) {}

  bool at_end() const { return at_ >= text_.size(); }
  char peek() const { return at_ < text_.size() ? text_[at_] : '\0'; }

  bool read_char(char& out) {
    if (at_end()) {
      return false;
    }
    out = text_[at_++];
    return true;
  }

  bool read_u64(u64& out) {
    const usize start = at_;
    u64 value = 0;
    while (!at_end() && text_[at_] >= '0' && text_[at_] <= '9') {
      value = value * 10 + static_cast<u64>(text_[at_] - '0');
      ++at_;
    }
    if (at_ == start) {
      return false;
    }
    out = value;
    return true;
  }

  bool read_name(std::string& out) {
    u64 length = 0;
    if (!read_u64(length) || text_.size() - at_ < length) {
      return false;
    }
    out.assign(text_.substr(at_, length));
    at_ += length;
    return true;
  }

  // A list of segments ends at the terminator, which is required so a
  // following length-prefixed item is not read as one more segment.
  bool read_segments(std::vector<std::string>& out) {
    out.clear();
    while (!at_end() && peek() != kListEnd) {
      std::string segment;
      if (!read_name(segment)) {
        return false;
      }
      out.push_back(std::move(segment));
    }
    char end = '\0';
    return read_char(end) && end == kListEnd;
  }

  // A list of types ends at the end of the symbol or at the first
  // byte that cannot begin a type, which is any digit: a length or a
  // count. Callers that must know where the list stops regardless of
  // what follows read an explicit count instead.
  bool type_list_ended() const {
    return at_end() || (peek() >= '0' && peek() <= '9');
  }

  bool decode_type(DecodedType& out) {
    char tag = '\0';
    if (!read_char(tag)) {
      return false;
    }
    if (const std::string_view spelling = primitive_spelling(tag);
        !spelling.empty()) {
      out.kind = DecodedType::Kind::Prim;
      out.spelling.assign(spelling);
      return true;
    }
    switch (tag) {
      case kTagStr: out.kind = DecodedType::Kind::Str; return true;
      case kTagPtr: out.kind = DecodedType::Kind::Ptr; return true;
      case kTagRef:
      case kTagMutRef: {
        out.kind =
            tag == kTagRef ? DecodedType::Kind::Ref : DecodedType::Kind::MutRef;
        out.parts.resize(1);
        return decode_type(out.parts[0]);
      }
      case kTagArray: {
        out.kind = DecodedType::Kind::Array;
        if (!read_u64(out.count)) {
          return false;
        }
        out.parts.resize(1);
        return decode_type(out.parts[0]);
      }
      case kTagTuple: {
        out.kind = DecodedType::Kind::Tuple;
        u64 count = 0;
        if (!read_u64(count) || !plausible_count(count)) {
          return false;
        }
        out.parts.resize(count);
        for (u64 i = 0; i < count; ++i) {
          if (!decode_type(out.parts[i])) {
            return false;
          }
        }
        return true;
      }
      case kTagNominal: {
        out.kind = DecodedType::Kind::Nominal;
        if (!read_segments(out.path)) {
          return false;
        }
        u64 count = 0;
        if (!read_u64(count) || !plausible_count(count)) {
          return false;
        }
        out.args.resize(count);
        for (u64 i = 0; i < count; ++i) {
          if (!decode_type(out.args[i])) {
            return false;
          }
        }
        return true;
      }
      default: return false;
    }
  }

  // Every element costs at least one byte, so a count larger than what
  // is left cannot be honest.
  bool plausible_count(u64 count) const { return count <= text_.size() - at_; }

 private:
  std::string_view text_;
  usize at_ = 0;
};

}  // namespace

std::string mangle(const Signature& signature,
                   const ir::Storage& types,
                   const str::StringInterner& strings) {
  if (signature.kind == Signature::Kind::Foreign) {
    // Not ours to encode: a C entry point keeps the name it was
    // declared with.
    return signature.name;
  }
  std::string out;
  out.append(kPrefix);
  out.append(kVersion);
  if (const char kind = kind_tag(signature.kind); kind != '\0') {
    out.push_back(kind);
  }
  append_path(out, signature.path);
  append_u64(out, signature.name.size());
  out.append(signature.name);
  Encoder encoder(types, strings, out);
  for (const ir::TypeIdx generic : signature.generics) {
    encoder.encode_type(generic);
  }
  return out;
}

bool demangle(std::string_view text, Demangled& out) {
  if (!text.starts_with(kPrefix)) {
    return false;
  }
  Decoder decoder(text.substr(kPrefix.size()));
  char version = '\0';
  if (!decoder.read_char(version) || version != kVersion[0]) {
    return false;
  }
  char kind = '\0';
  if (!decoder.read_char(kind)) {
    return false;
  }
  switch (kind) {
    case 'f': out.kind = Signature::Kind::Free; break;
    case 'a': out.kind = Signature::Kind::Assoc; break;
    case 'm': out.kind = Signature::Kind::Method; break;
    default: return false;
  }
  if (!decoder.read_segments(out.path) || !decoder.read_name(out.name)) {
    return false;
  }
  while (!decoder.type_list_ended()) {
    out.generics.emplace_back();
    if (!decoder.decode_type(out.generics.back())) {
      return false;
    }
  }
  // Trailing bytes mean the encoding and this decoder disagree.
  return decoder.at_end();
}

std::string display(const DecodedType& type) {
  switch (type.kind) {
    case DecodedType::Kind::Prim: return type.spelling;
    case DecodedType::Kind::Str: return "str";
    case DecodedType::Kind::Ptr: return "ptr";
    case DecodedType::Kind::Ref: return "&" + display(type.parts.at(0));
    case DecodedType::Kind::MutRef: return "&mut " + display(type.parts.at(0));
    case DecodedType::Kind::Array:
      return "[" + display(type.parts.at(0)) + "; " +
             std::to_string(type.count) + "]";
    case DecodedType::Kind::Tuple: {
      std::string out = "(";
      for (usize i = 0; i < type.parts.size(); ++i) {
        if (i != 0) {
          out.push_back(',');
        }
        out.append(display(type.parts[i]));
      }
      out.push_back(')');
      return out;
    }
    case DecodedType::Kind::Nominal: {
      std::string out;
      for (usize i = 0; i < type.path.size(); ++i) {
        if (i != 0) {
          out.append("::");
        }
        out.append(type.path[i]);
      }
      out.push_back('<');
      for (usize i = 0; i < type.args.size(); ++i) {
        if (i != 0) {
          out.push_back(',');
        }
        out.append(display(type.args[i]));
      }
      out.push_back('>');
      return out;
    }
  }
  return "?";
}

}  // namespace symbol
