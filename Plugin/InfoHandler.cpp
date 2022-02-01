/*
 * Copyright (c) 2022 Jon Palmisciano. All rights reserved.
 *
 * Use of this source code is governed by the BSD 3-Clause license; the full
 * terms of the license can be found in the LICENSE.txt file.
 */

#include "InfoHandler.h"

#include <regex>

using namespace BinaryNinja;

std::string InfoHandler::sanitizeText(const std::string& text)
{
    std::string result;
    std::string input = text.substr(0, 24);

    std::regex re("[a-zA-Z]+");
    std::smatch sm;
    while (std::regex_search(input, sm, re)) {
        std::string part = sm[0];
        part[0] = static_cast<char>(std::toupper(part[0]));

        result += part;
        input = sm.suffix();
    }

    return result;
}

TypeRef InfoHandler::namedType(BinaryViewRef bv, const std::string& name)
{
    return Type::NamedType(bv, name);
}

TypeRef InfoHandler::stringType(size_t size)
{
    return Type::ArrayType(Type::IntegerType(1, true), size + 1);
}

void InfoHandler::defineVariable(BinaryViewRef bv, uint64_t address, TypeRef type)
{
    bv->DefineUserDataVariable(address, type);
}

void InfoHandler::defineSymbol(BinaryViewRef bv, uint64_t address, const std::string& name,
    const std::string& prefix, BNSymbolType symbolType)
{
    bv->DefineUserSymbol(new Symbol(symbolType, prefix + name, address));
}

void InfoHandler::applyInfoToView(SharedAnalysisInfo info, BinaryViewRef bv)
{
    BinaryReader reader(bv);

    auto taggedPointerType = namedType(bv, "tptr_t");
    auto cfStringType = namedType(bv, "CFString");
    auto classType = namedType(bv, "objc_class_t");
    auto classDataType = namedType(bv, "objc_class_ro_t");
    auto methodListType = bv->GetTypeByName(std::string("objc_method_list_t"));
    auto methodType = bv->GetDefaultArchitecture()->GetName() == "aarch64"
        ? bv->GetTypeByName(std::string("objc_method_entry_t"))
        : bv->GetTypeByName(std::string("objc_method_t"));

    // Create data variables and symbols for all CFString instances.
    for (auto csi : info->cfStrings) {
        reader.Seek(csi.dataAddress);
        auto text = reader.ReadString(csi.size + 1);
        auto sanitizedText = sanitizeText(text);

        defineVariable(bv, csi.address, cfStringType);
        defineVariable(bv, csi.dataAddress, stringType(csi.size));
        defineSymbol(bv, csi.address, sanitizedText, "cf_");
        defineSymbol(bv, csi.dataAddress, sanitizedText, "as_");
    }

    // Create data variables and symbols for the analyzed classes.
    for (auto ci : info->classes) {
        defineVariable(bv, ci.listPointer, taggedPointerType);
        defineVariable(bv, ci.address, classType);
        defineVariable(bv, ci.dataAddress, classDataType);
        defineVariable(bv, ci.nameAddress, stringType(ci.name.size()));
        defineSymbol(bv, ci.listPointer, ci.name, "cp_");
        defineSymbol(bv, ci.address, ci.name, "cl_");
        defineSymbol(bv, ci.dataAddress, ci.name, "ro_");
        defineSymbol(bv, ci.nameAddress, ci.name, "nm_");

        auto mli = info->methodLists[ci.methodListAddress];
        if (mli.address == 0 || mli.methods.empty())
            continue;

        // Create data variables for each method in the method list.
        for (const auto& mi : mli.methods) {
            auto sel = info->selectorRefs[mi.nameAddress];

            defineVariable(bv, mi.address, methodType);
            defineVariable(bv, sel->nameAddress, stringType(sel->name.size()));
            defineSymbol(bv, mi.address, sel->name, "mt_");
            defineSymbol(bv, mi.nameAddress, sel->name, "sr_");
            defineSymbol(bv, sel->nameAddress, sel->name, "sn_");
        }

        // Create a data variable and symbol for the method list header.
        //
        // TODO: Create an anonymous structure for the entire method list rather
        // than creating separate variables for the header and each method.
        defineVariable(bv, ci.methodListAddress, methodListType);
        defineSymbol(bv, ci.methodListAddress, ci.name, "ml_");
    }
}
