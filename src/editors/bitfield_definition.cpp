// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "editors/bitfield_definition.h"

#include "win32/file_text.h"
#include "win32/shell_paths.h"
#include "win32/text_transform.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace regkit::editors::bitfield {

namespace {

constexpr wchar_t kFormat[] = L"regkit-bitfield";
constexpr wchar_t kExtension[] = L".regkit-bitfield.json";
constexpr int kMaxDepth = 8;

enum FileMember {
  kFileFormat = 1 << 0,
  kFileName = 1 << 1,
  kFileComment = 1 << 2,
  kFileDefinitions = 1 << 3,
};

enum DefinitionMember {
  kDefName = 1 << 0,
  kDefValueName = 1 << 1,
  kDefKeyPaths = 1 << 2,
  kDefBitWidth = 1 << 3,
  kDefByteOffset = 1 << 4,
  kDefComment = 1 << 5,
  kDefFields = 1 << 6,
};

enum FieldMember {
  kFieldName = 1 << 0,
  kFieldBits = 1 << 1,
  kFieldMeaning = 1 << 2,
  kFieldStates = 1 << 3,
};

enum StateMember {
  kStateValue = 1 << 0,
  kStateName = 1 << 1,
  kStateMeaning = 1 << 2,
};

class Parser {
public:
  Parser(std::wstring_view text, std::wstring* error)
      : ptr_(text.data()), end_(text.data() + text.size()), error_(error) {
  }

  bool ReadFile(DefinitionFile* file);

private:
  bool Fail(const wchar_t* message);
  void SkipSpace();
  bool Literal(wchar_t expected);
  bool ReadString(std::wstring* out, size_t limit);
  bool ReadUnsigned(uint64_t* out);
  bool ReadPaths(std::vector<std::wstring>* paths);
  bool ReadBits(std::vector<unsigned>* bits);
  bool ReadState(State* state);
  bool ReadStates(std::vector<State>* states);
  bool ReadField(Field* field);
  bool ReadFields(std::vector<Field>* fields);
  bool ReadDefinition(Definition* definition);
  bool ReadDefinitions(std::vector<Definition>* definitions);
  bool Hex4(unsigned* out);
  bool Enter();
  void Leave() {
    --depth_;
  }

  wchar_t Peek(size_t offset = 0) const {
    return static_cast<size_t>(end_ - ptr_) > offset ? ptr_[offset] : L'\0';
  }
  wchar_t PeekAdvance() {
    const wchar_t character = Peek();
    Advance();
    return character;
  }
  void Advance(size_t count = 1) {
    ptr_ += count < static_cast<size_t>(end_ - ptr_) ? count : static_cast<size_t>(end_ - ptr_);
  }

  const wchar_t* ptr_ = nullptr;
  const wchar_t* end_ = nullptr;
  std::wstring* error_ = nullptr;
  int depth_ = 0;
};

bool Parser::Fail(
    const wchar_t* message
) {
  if (error_ && error_->empty()) {
    *error_ = message;
  }
  return false;
}

bool Parser::Enter() {
  // limit nested arrays & objects
  if (++depth_ > kMaxDepth) {
    return Fail(L"The definition file is nested too deeply.");
  }
  return true;
}

void Parser::SkipSpace() {
  while (Peek() == L' ' || Peek() == L'\t' || Peek() == L'\r' || Peek() == L'\n') {
    Advance();
  }
}

bool Parser::Literal(
    wchar_t expected
) {
  SkipSpace();
  if (Peek() != expected) {
    return Fail(L"The definition file isn't valid JSON.");
  }
  Advance();
  return true;
}

bool Parser::Hex4(
    unsigned* out
) {
  unsigned value = 0;
  for (int i = 0; i < 4; ++i) {
    const wchar_t c = Peek(i);
    unsigned digit = 0;
    if (c >= L'0' && c <= L'9') {
      digit = static_cast<unsigned>(c - L'0');
    } else if (c >= L'a' && c <= L'f') {
      digit = static_cast<unsigned>(c - L'a') + 10;
    } else if (c >= L'A' && c <= L'F') {
      digit = static_cast<unsigned>(c - L'A') + 10;
    } else {
      return false;
    }
    value = (value << 4) | digit;
  }
  Advance(4);
  *out = value;
  return true;
}

bool Parser::ReadString(
    std::wstring* out,
    size_t limit
) {
  if (!Literal(L'"')) {
    return false;
  }
  out->clear();
  for (;;) {
    const wchar_t c = Peek();
    if (c == L'\0') {
      return Fail(L"The definition file ends inside a string.");
    }
    if (c == L'"') {
      Advance();
      break;
    }
    if (c < 0x20) {
      return Fail(L"A string contains an unescaped control character.");
    }
    if (c != L'\\') {
      out->push_back(c);
      Advance();
      continue;
    }
    Advance();
    const wchar_t escape = PeekAdvance();
    if (escape == L'\0') {
      return Fail(L"The definition file ends inside a string.");
    }
    switch (escape) {
    case L'"':
    case L'\\':
    case L'/':
      out->push_back(escape);
      break;
    case L'b':
      out->push_back(L'\b');
      break;
    case L'f':
      out->push_back(L'\f');
      break;
    case L'n':
      out->push_back(L'\n');
      break;
    case L'r':
      out->push_back(L'\r');
      break;
    case L't':
      out->push_back(L'\t');
      break;
    case L'u':
      {
        unsigned first = 0;
        if (!Hex4(&first)) {
          return Fail(L"A string contains a malformed escape.");
        }
        if (first >= 0xDC00 && first <= 0xDFFF) {
          return Fail(L"A string contains a lone surrogate.");
        }
        if (first >= 0xD800 && first <= 0xDBFF) {
          if (Peek(0) != L'\\' || Peek(1) != L'u') {
            return Fail(L"A string contains a lone surrogate.");
          }
          Advance(2);
          unsigned second = 0;
          if (!Hex4(&second) || second < 0xDC00 || second > 0xDFFF) {
            return Fail(L"A string contains a lone surrogate.");
          }
          out->push_back(static_cast<wchar_t>(first));
          out->push_back(static_cast<wchar_t>(second));
          break;
        }
        out->push_back(static_cast<wchar_t>(first));
        break;
      }
    default:
      return Fail(L"A string contains an unsupported escape.");
    }
    if (out->size() > limit) {
      return Fail(L"A text member is longer than the format allows.");
    }
  }
  if (out->size() > limit) {
    return Fail(L"A text member is longer than the format allows.");
  }
  return true;
}

bool Parser::ReadUnsigned(
    uint64_t* out
) {
  SkipSpace();
  if (Peek() < L'0' || Peek() > L'9') {
    return Fail(L"An unsigned number was expected.");
  }
  if (Peek(0) == L'0' && Peek(1) >= L'0' && Peek(1) <= L'9') {
    return Fail(L"A number has a leading zero.");
  }
  uint64_t value = 0;
  while (Peek() >= L'0' && Peek() <= L'9') {
    if (value > 0x0FFFFFFFFFFFFFFFull) {
      return Fail(L"A number is out of range.");
    }
    value = value * 10 + static_cast<uint64_t>(Peek() - L'0');
    Advance();
  }
  if (Peek() == L'.' || Peek() == L'e' || Peek() == L'E' || Peek() == L'-' || Peek() == L'+') {
    return Fail(L"Only unsigned integers are supported.");
  }
  *out = value;
  return true;
}

bool Parser::ReadPaths(
    std::vector<std::wstring>* paths
) {
  if (!Enter() || !Literal(L'[')) {
    return false;
  }
  SkipSpace();
  if (Peek() == L']') {
    Advance();
    Leave();
    return true;
  }
  for (;;) {
    if (paths->size() >= kMaxPaths) {
      return Fail(L"A definition lists too many key paths.");
    }
    std::wstring path;
    if (!ReadString(&path, kMaxPathLength)) {
      return false;
    }
    paths->push_back(std::move(path));
    SkipSpace();
    if (Peek() == L',') {
      Advance();
      continue;
    }
    break;
  }
  if (!Literal(L']')) {
    return false;
  }
  Leave();
  return true;
}

bool Parser::ReadBits(
    std::vector<unsigned>* bits
) {
  if (!Enter() || !Literal(L'[')) {
    return false;
  }
  SkipSpace();
  if (Peek() == L']') {
    Advance();
    return Fail(L"A field lists no bits.");
  }
  for (;;) {
    uint64_t value = 0;
    if (!ReadUnsigned(&value)) {
      return false;
    }
    if (value >= 64) {
      return Fail(L"A field uses a bit outside the declared width.");
    }
    if (!bits->empty() && value <= bits->back()) {
      return Fail(L"Field bits must be unique and in ascending order.");
    }
    bits->push_back(static_cast<unsigned>(value));
    SkipSpace();
    if (Peek() == L',') {
      Advance();
      continue;
    }
    break;
  }
  if (!Literal(L']')) {
    return false;
  }
  Leave();
  return true;
}

bool Parser::ReadState(
    State* state
) {
  if (!Enter() || !Literal(L'{')) {
    return false;
  }
  unsigned seen = 0;
  for (;;) {
    std::wstring member;
    if (!ReadString(&member, kMaxNameLength) || !Literal(L':')) {
      return false;
    }
    unsigned flag = 0;
    if (member == L"value") {
      flag = kStateValue;
      if (!ReadUnsigned(&state->value)) {
        return false;
      }
    } else if (member == L"name") {
      flag = kStateName;
      if (!ReadString(&state->name, kMaxNameLength)) {
        return false;
      }
    } else if (member == L"meaning") {
      flag = kStateMeaning;
      if (!ReadString(&state->meaning, kMaxMeaningLength)) {
        return false;
      }
    } else {
      return Fail(L"A state contains an unknown member.");
    }
    if (seen & flag) {
      return Fail(L"A state contains a duplicate member.");
    }
    seen |= flag;
    SkipSpace();
    if (Peek() == L',') {
      Advance();
      continue;
    }
    break;
  }
  if (!Literal(L'}')) {
    return false;
  }
  Leave();
  if ((seen & (kStateValue | kStateName)) != (kStateValue | kStateName)) {
    return Fail(L"A state is missing its value or name.");
  }
  return true;
}

bool Parser::ReadStates(
    std::vector<State>* states
) {
  if (!Enter() || !Literal(L'[')) {
    return false;
  }
  SkipSpace();
  if (Peek() == L']') {
    Advance();
    Leave();
    return true;
  }
  for (;;) {
    if (states->size() >= 256) {
      return Fail(L"A field lists more than 256 states.");
    }
    State state;
    if (!ReadState(&state)) {
      return false;
    }
    states->push_back(std::move(state));
    SkipSpace();
    if (Peek() == L',') {
      Advance();
      continue;
    }
    break;
  }
  if (!Literal(L']')) {
    return false;
  }
  Leave();
  return true;
}

bool Parser::ReadField(
    Field* field
) {
  if (!Enter() || !Literal(L'{')) {
    return false;
  }
  unsigned seen = 0;
  for (;;) {
    std::wstring member;
    if (!ReadString(&member, kMaxNameLength) || !Literal(L':')) {
      return false;
    }
    unsigned flag = 0;
    if (member == L"name") {
      flag = kFieldName;
      if (!ReadString(&field->name, kMaxNameLength)) {
        return false;
      }
    } else if (member == L"bits") {
      flag = kFieldBits;
      if (!ReadBits(&field->bits)) {
        return false;
      }
    } else if (member == L"meaning") {
      flag = kFieldMeaning;
      if (!ReadString(&field->meaning, kMaxMeaningLength)) {
        return false;
      }
    } else if (member == L"states") {
      flag = kFieldStates;
      if (!ReadStates(&field->states)) {
        return false;
      }
    } else {
      return Fail(L"A field contains an unknown member.");
    }
    if (seen & flag) {
      return Fail(L"A field contains a duplicate member.");
    }
    seen |= flag;
    SkipSpace();
    if (Peek() == L',') {
      Advance();
      continue;
    }
    break;
  }
  if (!Literal(L'}')) {
    return false;
  }
  Leave();
  if ((seen & (kFieldName | kFieldBits)) != (kFieldName | kFieldBits)) {
    return Fail(L"A field is missing its name or bits.");
  }
  return true;
}

bool Parser::ReadFields(
    std::vector<Field>* fields
) {
  if (!Enter() || !Literal(L'[')) {
    return false;
  }
  SkipSpace();
  if (Peek() == L']') {
    Advance();
    Leave();
    return true;
  }
  for (;;) {
    if (fields->size() >= 64) {
      return Fail(L"A definition contains more than 64 fields.");
    }
    Field field;
    if (!ReadField(&field)) {
      return false;
    }
    fields->push_back(std::move(field));
    SkipSpace();
    if (Peek() == L',') {
      Advance();
      continue;
    }
    break;
  }
  if (!Literal(L']')) {
    return false;
  }
  Leave();
  return true;
}

bool Parser::ReadDefinition(
    Definition* definition
) {
  if (!Enter() || !Literal(L'{')) {
    return false;
  }
  unsigned seen = 0;
  for (;;) {
    std::wstring member;
    if (!ReadString(&member, kMaxNameLength) || !Literal(L':')) {
      return false;
    }
    unsigned flag = 0;
    if (member == L"name") {
      flag = kDefName;
      if (!ReadString(&definition->name, kMaxNameLength)) {
        return false;
      }
    } else if (member == L"value_name") {
      flag = kDefValueName;
      if (!ReadString(&definition->value_name, kMaxNameLength)) {
        return false;
      }
    } else if (member == L"key_paths") {
      flag = kDefKeyPaths;
      if (!ReadPaths(&definition->key_paths)) {
        return false;
      }
    } else if (member == L"bit_width") {
      flag = kDefBitWidth;
      uint64_t width = 0;
      if (!ReadUnsigned(&width)) {
        return false;
      }
      if (!ValidWidth(static_cast<unsigned>(width))) {
        return Fail(L"The bit width must be 8, 16, 32, or 64.");
      }
      definition->bit_width = static_cast<unsigned>(width);
    } else if (member == L"byte_offset") {
      flag = kDefByteOffset;
      uint64_t offset = 0;
      if (!ReadUnsigned(&offset)) {
        return false;
      }
      if (offset > kMaxByteOffset) {
        return Fail(L"The byte offset is out of range.");
      }
      definition->byte_offset = static_cast<unsigned>(offset);
    } else if (member == L"comment") {
      flag = kDefComment;
      if (!ReadString(&definition->comment, kMaxCommentLength)) {
        return false;
      }
    } else if (member == L"fields") {
      flag = kDefFields;
      if (!ReadFields(&definition->fields)) {
        return false;
      }
    } else {
      return Fail(L"A definition contains an unknown member.");
    }
    if (seen & flag) {
      return Fail(L"A definition contains a duplicate member.");
    }
    seen |= flag;
    SkipSpace();
    if (Peek() == L',') {
      Advance();
      continue;
    }
    break;
  }
  if (!Literal(L'}')) {
    return false;
  }
  Leave();
  constexpr unsigned required = kDefValueName | kDefBitWidth;
  if ((seen & required) != required) {
    return Fail(L"A definition is missing its value name or bit width.");
  }
  return true;
}

bool Parser::ReadDefinitions(
    std::vector<Definition>* definitions
) {
  if (!Enter() || !Literal(L'[')) {
    return false;
  }
  SkipSpace();
  if (Peek() == L']') {
    Advance();
    return Fail(L"The file contains no definitions.");
  }
  for (;;) {
    Definition definition;
    if (!ReadDefinition(&definition)) {
      return false;
    }
    definitions->push_back(std::move(definition));
    SkipSpace();
    if (Peek() == L',') {
      Advance();
      continue;
    }
    break;
  }
  if (!Literal(L']')) {
    return false;
  }
  Leave();
  return true;
}

bool Parser::ReadFile(
    DefinitionFile* file
) {
  if (!Literal(L'{')) {
    return false;
  }
  unsigned seen = 0;
  for (;;) {
    std::wstring member;
    if (!ReadString(&member, kMaxNameLength) || !Literal(L':')) {
      return false;
    }
    unsigned flag = 0;
    if (member == L"format") {
      flag = kFileFormat;
      std::wstring format;
      if (!ReadString(&format, kMaxNameLength)) {
        return false;
      }
      if (format != kFormat) {
        return Fail(L"The file isn't a RegKit bitfield definition.");
      }
    } else if (member == L"name") {
      flag = kFileName;
      if (!ReadString(&file->name, kMaxNameLength)) {
        return false;
      }
    } else if (member == L"comment") {
      flag = kFileComment;
      if (!ReadString(&file->comment, kMaxCommentLength)) {
        return false;
      }
    } else if (member == L"definitions") {
      flag = kFileDefinitions;
      if (!ReadDefinitions(&file->definitions)) {
        return false;
      }
    } else {
      return Fail(L"The file contains an unknown member.");
    }
    if (seen & flag) {
      return Fail(L"The file contains a duplicate member.");
    }
    seen |= flag;
    SkipSpace();
    if (Peek() == L',') {
      Advance();
      continue;
    }
    break;
  }
  if (!Literal(L'}')) {
    return false;
  }
  SkipSpace();
  if (Peek() != L'\0') {
    return Fail(L"The definition file contains trailing content.");
  }
  constexpr unsigned required = kFileFormat | kFileDefinitions;
  if ((seen & required) != required) {
    return Fail(L"The file is missing a required member.");
  }
  return true;
}

void AppendEscaped(
    std::wstring* out,
    const std::wstring& text
) {
  out->push_back(L'"');
  for (size_t i = 0; i < text.size(); ++i) {
    const wchar_t c = text[i];
    switch (c) {
    case L'"':
      out->append(L"\\\"");
      continue;
    case L'\\':
      out->append(L"\\\\");
      continue;
    case L'\b':
      out->append(L"\\b");
      continue;
    case L'\f':
      out->append(L"\\f");
      continue;
    case L'\n':
      out->append(L"\\n");
      continue;
    case L'\r':
      out->append(L"\\r");
      continue;
    case L'\t':
      out->append(L"\\t");
      continue;
    default:
      break;
    }
    bool escape = c < 0x20 || c == 0x7F;
    if (c >= 0xD800 && c <= 0xDBFF) {
      const wchar_t next = i + 1 < text.size() ? text[i + 1] : 0;
      if (next >= 0xDC00 && next <= 0xDFFF) {
        out->push_back(c);
        out->push_back(next);
        ++i;
        continue;
      }
      escape = true;
    } else if (c >= 0xDC00 && c <= 0xDFFF) {
      escape = true;
    }
    if (escape) {
      wchar_t buffer[8] = {};
      swprintf_s(buffer, L"\\u%04X", static_cast<unsigned>(c));
      out->append(buffer);
      continue;
    }
    out->push_back(c);
  }
  out->push_back(L'"');
}

void AppendMember(
    std::wstring* out,
    const wchar_t* indent,
    const wchar_t* name,
    const std::wstring& text
) {
  out->append(indent).append(L"\"").append(name).append(L"\": ");
  AppendEscaped(out, text);
}

std::wstring BundleDirectory() {
  const std::wstring module_dir = util::GetModuleDirectory();
  if (module_dir.empty()) {
    return L"";
  }
  return util::JoinPath(util::JoinPath(module_dir, L"assets"), L"bitfields");
}

std::vector<DefinitionFile> LoadBundledFiles() {
  std::vector<DefinitionFile> files;
  const std::wstring directory = BundleDirectory();
  if (directory.empty()) {
    return files;
  }
  WIN32_FIND_DATAW found = {};
  const HANDLE search = FindFirstFileW(util::JoinPath(directory, L"*.regkit-bitfield.json").c_str(), &found);
  if (search == INVALID_HANDLE_VALUE) {
    return files;
  }
  do {
    if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      continue;
    }
    DefinitionFile file;
    std::wstring error;
    // ignore invalid files without hiding remaining definitions
    if (Load(util::JoinPath(directory, found.cFileName), &file, &error)) {
      files.push_back(std::move(file));
    }
  } while (FindNextFileW(search, &found));
  FindClose(search);
  std::stable_sort(
      files.begin(),
      files.end(),
      [](const DefinitionFile& left, const DefinitionFile& right) {
        return util::CompareInsensitive(left.name, right.name) < 0;
      }
  );
  return files;
}

} // namespace

uint64_t Field::Extract(
    uint64_t value
) const {
  uint64_t out = 0;
  for (size_t i = 0; i < bits.size(); ++i) {
    if ((value >> bits[i]) & 1ull) {
      out |= 1ull << i;
    }
  }
  return out;
}

uint64_t Field::Apply(
    uint64_t value,
    uint64_t field_value
) const {
  for (size_t i = 0; i < bits.size(); ++i) {
    const uint64_t bit = 1ull << bits[i];
    if ((field_value >> i) & 1ull) {
      value |= bit;
    } else {
      value &= ~bit;
    }
  }
  return value;
}

const State* Field::StateFor(
    uint64_t field_value
) const {
  for (const State& state : states) {
    if (state.value == field_value) {
      return &state;
    }
  }
  return nullptr;
}

int Definition::FieldIndexForBit(
    unsigned bit
) const {
  if (bit >= 64) {
    return -1;
  }
  const signed char index = owner[bit];
  if (index < 0 || static_cast<size_t>(index) >= fields.size()) {
    return -1;
  }
  return index;
}

const Field* Definition::FieldForBit(
    unsigned bit
) const {
  const int index = FieldIndexForBit(bit);
  return index < 0 ? nullptr : &fields[static_cast<size_t>(index)];
}

bool Definition::MatchesPath(
    const std::wstring& key_path
) const {
  // no path filters means definition applies to every matching value name
  if (key_paths.empty()) {
    return true;
  }
  for (const std::wstring& fragment : key_paths) {
    if (util::ContainsInsensitive(key_path, fragment)) {
      return true;
    }
  }
  return false;
}

std::wstring DisplayName(
    const Definition& definition
) {
  if (!definition.name.empty()) {
    return definition.name;
  }
  if (!definition.value_name.empty()) {
    return definition.value_name;
  }
  return L"Unnamed definition";
}

bool ValidWidth(
    unsigned bit_width
) {
  return bit_width == 8 || bit_width == 16 || bit_width == 32 || bit_width == 64;
}

uint64_t WidthMask(
    unsigned bit_width
) {
  if (bit_width >= 64) {
    return ~0ull;
  }
  return (1ull << bit_width) - 1ull;
}

void BuildLookup(
    Definition* definition
) {
  definition->owner.fill(-1);
  for (size_t i = 0; i < definition->fields.size(); ++i) {
    Field& field = definition->fields[i];
    field.mask = 0;
    for (const unsigned bit : field.bits) {
      if (bit < 64) {
        field.mask |= 1ull << bit;
        definition->owner[bit] = static_cast<signed char>(i);
      }
    }
  }
}

bool Validate(
    Definition* definition,
    std::wstring* error
) {
  const auto fail = [error](const wchar_t* message) {
    if (error) {
      *error = message;
    }
    return false;
  };
  if (!definition) {
    return false;
  }
  if (!ValidWidth(definition->bit_width)) {
    return fail(L"The bit width must be 8, 16, 32, or 64.");
  }
  if (definition->byte_offset > kMaxByteOffset) {
    return fail(L"The byte offset is out of range.");
  }
  if (definition->fields.size() > 64) {
    return fail(L"A definition contains more than 64 fields.");
  }
  if (definition->name.size() > kMaxNameLength ||
      definition->value_name.size() > kMaxNameLength) {
    return fail(L"A name is longer than the format allows.");
  }
  if (definition->comment.size() > kMaxCommentLength) {
    return fail(L"A comment is longer than the format allows.");
  }
  if (definition->key_paths.size() > kMaxPaths) {
    return fail(L"A definition lists too many key paths.");
  }
  for (const std::wstring& path : definition->key_paths) {
    if (path.empty() || path.size() > kMaxPathLength) {
      return fail(L"A key path is empty or longer than the format allows.");
    }
  }
  std::array<signed char, 64> claimed = {};
  claimed.fill(-1);
  for (size_t i = 0; i < definition->fields.size(); ++i) {
    Field& field = definition->fields[i];
    if (field.name.empty() || field.name.size() > kMaxNameLength) {
      return fail(L"Every field needs a name of at most 256 characters.");
    }
    if (field.meaning.size() > kMaxMeaningLength) {
      return fail(L"A field meaning is longer than the format allows.");
    }
    if (field.bits.empty()) {
      return fail(L"Every field needs at least one bit.");
    }
    for (size_t j = 0; j < field.bits.size(); ++j) {
      if (field.bits[j] >= definition->bit_width) {
        return fail(L"A field uses a bit outside the declared width.");
      }
      if (j > 0 && field.bits[j] <= field.bits[j - 1]) {
        return fail(L"Field bits must be unique and in ascending order.");
      }
      if (claimed[field.bits[j]] >= 0) {
        return fail(L"Two fields claim the same bit.");
      }
      claimed[field.bits[j]] = static_cast<signed char>(i);
    }
    const uint64_t limit = WidthMask(static_cast<unsigned>(field.bits.size()));
    for (size_t j = 0; j < field.states.size(); ++j) {
      const State& state = field.states[j];
      if (state.name.empty() || state.name.size() > kMaxNameLength) {
        return fail(L"Every state needs a name of at most 256 characters.");
      }
      if (state.meaning.size() > kMaxMeaningLength) {
        return fail(L"A state meaning is longer than the format allows.");
      }
      if (state.value > limit) {
        return fail(L"A state value doesn't fit the bits of its field.");
      }
      for (size_t k = 0; k < j; ++k) {
        if (field.states[k].value == state.value) {
          return fail(L"Two states of one field share the same value.");
        }
      }
    }
    for (size_t j = 0; j < i; ++j) {
      if (util::EqualsInsensitive(definition->fields[j].name, field.name)) {
        return fail(L"Two fields share the same name.");
      }
    }
  }
  std::stable_sort(
      definition->fields.begin(),
      definition->fields.end(),
      [](const Field& left, const Field& right) {
        return left.bits.front() < right.bits.front();
      }
  );
  for (Field& field : definition->fields) {
    std::stable_sort(
        field.states.begin(),
        field.states.end(),
        [](const State& left, const State& right) { return left.value < right.value; }
    );
  }
  BuildLookup(definition);
  return true;
}

bool Validate(
    DefinitionFile* file,
    std::wstring* error
) {
  if (!file) {
    return false;
  }
  if (file->definitions.empty()) {
    if (error) {
      *error = L"The file contains no definitions.";
    }
    return false;
  }
  if (file->name.size() > kMaxNameLength || file->comment.size() > kMaxCommentLength) {
    if (error) {
      *error = L"A file name or comment is longer than the format allows.";
    }
    return false;
  }
  for (Definition& definition : file->definitions) {
    if (!Validate(&definition, error)) {
      return false;
    }
  }
  return true;
}

bool Parse(
    const std::vector<BYTE>& utf8,
    DefinitionFile* file,
    std::wstring* error
) {
  if (!file) {
    return false;
  }
  const BYTE* data = utf8.data();
  size_t size = utf8.size();
  // accept an optional UTF8 byte order mark
  if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
    data += 3;
    size -= 3;
  }
  std::wstring text;
  if (size > 0) {
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(data), static_cast<int>(size), nullptr, 0);
    if (needed <= 0) {
      if (error) {
        *error = L"The definition file isn't valid UTF-8.";
      }
      return false;
    }
    text.resize(static_cast<size_t>(needed));
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(data), static_cast<int>(size), text.data(), needed);
  }
  DefinitionFile parsed;
  std::wstring message;
  Parser parser(text, &message);
  if (!parser.ReadFile(&parsed)) {
    if (error) {
      *error = message.empty() ? L"The definition file isn't valid JSON." : message;
    }
    return false;
  }
  if (!Validate(&parsed, error)) {
    return false;
  }
  *file = std::move(parsed);
  return true;
}

bool Load(
    const std::wstring& path,
    DefinitionFile* file,
    std::wstring* error
) {
  std::vector<BYTE> bytes;
  if (!util::ReadFileBytes(path, &bytes, kMaxFileBytes)) {
    if (error) {
      *error = L"The definition file couldn't be read.";
    }
    return false;
  }
  if (!Parse(bytes, file, error)) {
    return false;
  }
  file->path = path;
  return true;
}

std::wstring Serialize(
    const DefinitionFile& file
) {
  std::wstring out;
  out.append(L"{\n  \"format\": \"").append(kFormat).append(L"\",\n");
  if (!file.name.empty()) {
    AppendMember(&out, L"  ", L"name", file.name);
    out.append(L",\n");
  }
  if (!file.comment.empty()) {
    AppendMember(&out, L"  ", L"comment", file.comment);
    out.append(L",\n");
  }
  out.append(L"  \"definitions\": [\n");
  for (size_t d = 0; d < file.definitions.size(); ++d) {
    const Definition& definition = file.definitions[d];
    out.append(L"    {\n");
    if (!definition.name.empty()) {
      AppendMember(&out, L"      ", L"name", definition.name);
      out.append(L",\n");
    }
    AppendMember(&out, L"      ", L"value_name", definition.value_name);
    if (!definition.key_paths.empty()) {
      out.append(L",\n      \"key_paths\": [\n");
      for (size_t i = 0; i < definition.key_paths.size(); ++i) {
        out.append(L"        ");
        AppendEscaped(&out, definition.key_paths[i]);
        out.append(i + 1 < definition.key_paths.size() ? L",\n" : L"\n");
      }
      out.append(L"      ]");
    }
    out.append(L",\n      \"bit_width\": ").append(std::to_wstring(definition.bit_width));
    if (definition.byte_offset != 0) {
      out.append(L",\n      \"byte_offset\": ").append(std::to_wstring(definition.byte_offset));
    }
    if (!definition.comment.empty()) {
      out.append(L",\n");
      AppendMember(&out, L"      ", L"comment", definition.comment);
    }
    if (!definition.fields.empty()) {
      out.append(L",\n      \"fields\": [\n");
      for (size_t i = 0; i < definition.fields.size(); ++i) {
        const Field& field = definition.fields[i];
        out.append(L"        {\n");
        AppendMember(&out, L"          ", L"name", field.name);
        out.append(L",\n          \"bits\": [");
        for (size_t j = 0; j < field.bits.size(); ++j) {
          if (j > 0) {
            out.append(L", ");
          }
          out.append(std::to_wstring(field.bits[j]));
        }
        out.append(L"]");
        if (!field.meaning.empty()) {
          out.append(L",\n");
          AppendMember(&out, L"          ", L"meaning", field.meaning);
        }
        if (!field.states.empty()) {
          out.append(L",\n          \"states\": [\n");
          for (size_t j = 0; j < field.states.size(); ++j) {
            const State& state = field.states[j];
            out.append(L"            { \"value\": ").append(std::to_wstring(state.value)).append(L", ");
            AppendMember(&out, L"", L"name", state.name);
            if (!state.meaning.empty()) {
              out.append(L", ");
              AppendMember(&out, L"", L"meaning", state.meaning);
            }
            out.append(L" }");
            out.append(j + 1 < field.states.size() ? L",\n" : L"\n");
          }
          out.append(L"          ]");
        }
        out.append(L"\n        }");
        out.append(i + 1 < definition.fields.size() ? L",\n" : L"\n");
      }
      out.append(L"      ]");
    }
    out.append(L"\n    }");
    out.append(d + 1 < file.definitions.size() ? L",\n" : L"\n");
  }
  out.append(L"  ]\n}\n");
  return out;
}

bool Save(
    const std::wstring& path,
    const DefinitionFile& file,
    std::wstring* error
) {
  DefinitionFile copy = file;
  if (!Validate(&copy, error)) {
    return false;
  }
  if (!util::WriteTextFile(path, Serialize(copy), false)) {
    if (error) {
      *error = L"The definition file couldn't be written.";
    }
    return false;
  }
  return true;
}

const std::vector<DefinitionFile>& BundledFiles() {
  static const std::vector<DefinitionFile> files = LoadBundledFiles();
  return files;
}

std::vector<Definition> Matching(
    const std::wstring& key_path,
    const std::wstring& value_name
) {
  std::vector<Definition> matches;
  for (const DefinitionFile& file : BundledFiles()) {
    for (const Definition& definition : file.definitions) {
      if (!util::EqualsInsensitive(definition.value_name, value_name)) {
        continue;
      }
      if (!definition.MatchesPath(key_path)) {
        continue;
      }
      matches.push_back(definition);
    }
  }
  return matches;
}

std::wstring SuggestedFileName(
    const std::wstring& value_name
) {
  std::wstring base = value_name.empty() ? L"Default" : value_name;
  for (wchar_t& c : base) {
    if (c < 0x20 || wcschr(L"<>:\"/\\|?*", c)) {
      c = L'_';
    }
  }
  return base + kExtension;
}

const wchar_t* FileFilter() {
  return L"RegKit bitfield definitions (*.regkit-bitfield.json)\0*.regkit-bitfield.json\0JSON files (*.json)\0*.json\0";
}

const wchar_t* FileExtension() {
  return kExtension;
}

} // namespace regkit::editors::bitfield
