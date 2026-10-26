// Copyright (C) 2026 nohuto
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "records/json.h"

#include "win32/translation.h"

#include <string_view>

namespace regkit::json
{

namespace
{

void ReportError(void* context, const win32ui::JsonDiagnostic& diagnostic, std::wstring_view custom_text)
{
    auto* destination = static_cast<std::wstring*>(context);
    if (!destination || !destination->empty())
    {
        return;
    }
    switch (diagnostic.code)
    {
    case win32ui::JsonErrorCode::kNone:
        return;
    case win32ui::JsonErrorCode::kExpectedCharacter:
    case win32ui::JsonErrorCode::kUnterminatedComment:
    case win32ui::JsonErrorCode::kUnexpectedToken:
    case win32ui::JsonErrorCode::kBooleanExpected:
    case win32ui::JsonErrorCode::kEmptyFraction:
    case win32ui::JsonErrorCode::kEmptyExponent:
        *destination = util::Tr(L"The file isn't valid JSON.");
        break;
    case win32ui::JsonErrorCode::kTooDeep:
        *destination = util::Tr(L"The file is nested too deeply.");
        break;
    case win32ui::JsonErrorCode::kStringTooLong:
        *destination = util::Tr(L"A text member is longer than the format allows.");
        break;
    case win32ui::JsonErrorCode::kUnterminatedString:
        *destination = util::Tr(L"The file ends inside a string.");
        break;
    case win32ui::JsonErrorCode::kUnescapedControl:
        *destination = util::Tr(L"A string contains an unescaped control character.");
        break;
    case win32ui::JsonErrorCode::kMalformedEscape:
        *destination = util::Tr(L"A string contains a malformed escape.");
        break;
    case win32ui::JsonErrorCode::kUnsupportedEscape:
        *destination = util::Tr(L"A string contains an unsupported escape.");
        break;
    case win32ui::JsonErrorCode::kUnsignedExpected:
        *destination = util::Tr(L"An unsigned number was expected.");
        break;
    case win32ui::JsonErrorCode::kLeadingZero:
        *destination = util::Tr(L"A number has a leading zero.");
        break;
    case win32ui::JsonErrorCode::kNumberOutOfRange:
        *destination = util::Tr(L"A number is out of range.");
        break;
    case win32ui::JsonErrorCode::kIntegersOnly:
        *destination = util::Tr(L"Only unsigned integers are supported.");
        break;
    case win32ui::JsonErrorCode::kTrailingContent:
        *destination = util::Tr(L"The file contains trailing content.");
        break;
    case win32ui::JsonErrorCode::kCustom:
        *destination = custom_text;
        break;
    }
}

} // namespace

win32ui::JsonErrorSink Errors(std::wstring* destination)
{
    return {ReportError, destination};
}

} // namespace regkit::json
