/*
 * Copyright (c) 2022-2023 Jon Palmisciano. All rights reserved.
 *
 * Use of this source code is governed by the BSD 3-Clause license; the full
 * terms of the license can be found in the LICENSE.txt file.
 */

#include "InfoHandler.h"

#include "Constants.h"
#include "CustomTypes.h"
#include "Performance.h"

#include <algorithm>
#include <cinttypes>
#include <regex>

using namespace BinaryNinja;

std::string InfoHandler::sanitizeText(const std::string& text)
{
    std::string result;
    std::string input = text.substr(0, 24);

    std::regex re("[a-zA-Z0-9]+");
    std::smatch sm;
    while (std::regex_search(input, sm, re)) {
        std::string part = sm[0];
        part[0] = static_cast<char>(std::toupper(part[0]));

        result += part;
        input = sm.suffix();
    }

    return result;
}

std::string InfoHandler::sanitizeSelector(const std::string& text)
{
    auto result = text;
    std::replace(result.begin(), result.end(), ':', '_');

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

void InfoHandler::defineReference(BinaryViewRef bv, uint64_t from, uint64_t to)
{
    bv->AddUserDataReference(from, to);
}

void InfoHandler::applyMethodType(BinaryViewRef bv, const ObjectiveNinja::ClassInfo& ci,
    const BinaryNinja::QualifiedName& classTypeName, const ObjectiveNinja::MethodInfo& mi)
{
    auto selectorTokens = mi.selectorTokens();
    std::vector<ObjectiveNinja::QualifiedNameOrType> typeTokens = mi.decodedTypeTokens();

    // For safety, ensure out-of-bounds indexing is not about to occur. This has
    // never happened and likely won't ever happen, but crashing the product is
    // generally undesirable, so it's better to be safe than sorry.
    if (selectorTokens.size() > typeTokens.size()) {
        LogWarn("Cannot apply method type to %" PRIx64 " due to selector/type token size mismatch.", mi.implAddress);
        return;
    }

    auto typeForQualifiedNameOrType = [bv](ObjectiveNinja::QualifiedNameOrType nameOrType) {
        Ref<Type> type;

        if (nameOrType.type) {
            type = nameOrType.type;
            if (!type)
                type = Type::PointerType(bv->GetAddressSize(), Type::VoidType());

        } else {
            type = Type::NamedType(nameOrType.name, Type::PointerType(bv->GetAddressSize(), Type::VoidType()));
            for (size_t i = nameOrType.ptrCount; i > 0; i--)
                type = Type::PointerType(8, type);
        }

        return type;
    };

    BinaryNinja::QualifiedNameAndType nameAndType;
    std::set<BinaryNinja::QualifiedName> typesAllowRedefinition;

    auto retType = typeForQualifiedNameOrType(typeTokens[0]);

    std::vector<BinaryNinja::FunctionParameter> params;
    auto cc = bv->GetDefaultPlatform()->GetDefaultCallingConvention();

    params.push_back({ "self",
        classTypeName.IsEmpty()
            ? BinaryNinja::Type::NamedType(bv, { "id" })
            : BinaryNinja::Type::NamedType(bv, classTypeName),
        true,
        BinaryNinja::Variable() });

    params.push_back({ "sel",
        namedType(bv, "SEL"),
        true,
        BinaryNinja::Variable() });

    for (size_t i = 3; i < typeTokens.size(); i++) {
        std::string suffix;

        params.push_back({ selectorTokens.size() > i - 3 ? selectorTokens[i - 3] : "arg",
            typeForQualifiedNameOrType(typeTokens[i]),
            true,
            BinaryNinja::Variable() });
    }

    auto funcType = BinaryNinja::Type::FunctionType(retType, cc, params);

    // Search for the method's implementation function; apply the type if found.
    auto f = bv->GetAnalysisFunction(bv->GetDefaultPlatform(), mi.implAddress);
    if (f)
        f->SetUserType(funcType);
    else
        BNLogError("Processing Type for function at %llx failed", mi.implAddress);

    std::string prefix = ci.isMetaClass ? "+" : "-";

    auto name = prefix + "[" + ci.name + " " + mi.selector + "]";
    defineSymbol(bv, mi.implAddress, name, "", FunctionSymbol);
}

QualifiedName InfoHandler::createClassType(BinaryViewRef bv, const ObjectiveNinja::ClassInfo& info, const ObjectiveNinja::IvarListInfo& vi)
{
    StructureBuilder classTypeBuilder;
    for (const auto& ivar : vi.ivars) {
        ObjectiveNinja::QualifiedNameOrType encodedType = ivar.decodedTypeToken();
        Ref<Type> type;

        if (encodedType.type)
            type = encodedType.type;
        else
        {
            type = Type::NamedType(encodedType.name, Type::PointerType(bv->GetAddressSize(), Type::VoidType()));
            for (size_t i = encodedType.ptrCount; i > 0; i--)
                type = Type::PointerType(8, type);
        }

        if (!type)
            type = Type::PointerType(bv->GetAddressSize(), Type::VoidType());

        classTypeBuilder.AddMemberAtOffset(type, ivar.name, ivar.offset);
    }

    auto classTypeStruct = classTypeBuilder.Finalize();
    QualifiedName classTypeName = "class_" + std::string(info.name);
    std::string classTypeId = Type::GenerateAutoTypeId("objc", classTypeName);
    Ref<Type> classType = Type::StructureType(classTypeStruct);
    QualifiedName classQualName = bv->DefineType(classTypeId, classTypeName, classType);

    std::string typeID = Type::GenerateAutoTypeId("objc", info.name);
    bv->DefineType(typeID, info.name, Type::PointerType(bv->GetAddressSize(), Type::NamedType(bv, classTypeName)));

    return info.name;
}

void InfoHandler::applyInfoToView(SharedAnalysisInfo info, BinaryViewRef bv)
{
    auto start = Performance::now();

    bv->BeginUndoActions();

    BinaryReader reader(bv);

    auto taggedPointerType = namedType(bv, CustomTypes::TaggedPointer);
    auto cfStringType = namedType(bv, CustomTypes::CFString);
    auto classType = namedType(bv, CustomTypes::Class);
    auto classDataType = namedType(bv, CustomTypes::ClassRO);
    auto methodListType = namedType(bv, CustomTypes::MethodList);
    auto ivarListType = namedType(bv, CustomTypes::IvarList);
    auto ivarType = namedType(bv, CustomTypes::Ivar);

    auto objcComponent = bv->CreateComponentWithName("Objective-C Classes");

    // Create data variables and symbols for all CFString instances.
    for (const auto& csi : info->cfStrings) {
        reader.Seek(csi.dataAddress);
        auto text = reader.ReadString(csi.size + 1);
        auto sanitizedText = sanitizeText(text);

        defineVariable(bv, csi.address, cfStringType);
        defineVariable(bv, csi.dataAddress, stringType(csi.size));
        defineSymbol(bv, csi.address, sanitizedText, "cf_");
        defineSymbol(bv, csi.dataAddress, sanitizedText, "as_");

        defineReference(bv, csi.address, csi.dataAddress);
    }

    // Create data variables and symbols for selectors and selector references.
    for (const auto& sr : info->selectorRefs) {
        auto sanitizedSelector = sanitizeSelector(sr->name);

        defineVariable(bv, sr->address, taggedPointerType);
        defineVariable(bv, sr->nameAddress, stringType(sr->name.size()));
        defineSymbol(bv, sr->address, sanitizedSelector, "sr_");
        defineSymbol(bv, sr->nameAddress, sanitizedSelector, "sl_");

        defineReference(bv, sr->address, sr->nameAddress);
    }

    unsigned totalMethods = 0;

    std::map<uint64_t, std::string> addressToClassMap;

    // Create data variables and symbols for the analyzed classes.
    for (const auto& ci : info->classes) {
        auto classComponent = bv->CreateComponentWithName(ci.name, objcComponent);
        defineVariable(bv, ci.listPointer, taggedPointerType);
        defineVariable(bv, ci.address, classType);
        defineVariable(bv, ci.dataAddress, classDataType);
        defineVariable(bv, ci.nameAddress, stringType(ci.name.size()));
        classComponent->AddDataVariable({ci.listPointer, taggedPointerType, true});
        classComponent->AddDataVariable({ci.address, classType, true});
        classComponent->AddDataVariable({ci.dataAddress, classDataType, true});
        classComponent->AddDataVariable({ci.nameAddress, stringType(ci.name.size()), true});
        defineSymbol(bv, ci.listPointer, ci.name, "cp_");
        defineSymbol(bv, ci.address, ci.name, "cl_");
        addressToClassMap[ci.address] = ci.name;
        defineSymbol(bv, ci.dataAddress, ci.name, "ro_");
        defineSymbol(bv, ci.nameAddress, ci.name, "nm_");

        defineReference(bv, ci.listPointer, ci.address);
        defineReference(bv, ci.address, ci.dataAddress);
        defineReference(bv, ci.dataAddress, ci.nameAddress);
        defineReference(bv, ci.dataAddress, ci.methodListAddress);

        auto methodSelfType = createClassType(bv, ci, ci.ivarList);

        if (ci.methodList.address == 0 || ci.methodList.methods.empty())
            continue;

        auto methodType = ci.methodList.hasRelativeOffsets()
            ? bv->GetTypeByName(CustomTypes::MethodListEntry)
            : bv->GetTypeByName(CustomTypes::Method);

        // Create data variables for each method in the method list.
        for (const auto& mi : ci.methodList.methods) {
            ++totalMethods;

            defineVariable(bv, mi.address, methodType);
            defineSymbol(bv, mi.address, sanitizeSelector(mi.selector), "mt_");
            defineVariable(bv, mi.typeAddress, stringType(mi.type.size()));

            defineReference(bv, ci.methodList.address, mi.address);
            defineReference(bv, mi.address, mi.nameAddress);
            defineReference(bv, mi.address, mi.typeAddress);
            defineReference(bv, mi.address, mi.implAddress);

            applyMethodType(bv, ci, methodSelfType, mi);

            classComponent->AddFunction(bv->GetAnalysisFunction(bv->GetDefaultPlatform(), mi.implAddress));
        }

        if (ci.ivarListAddress != 0) {
            defineVariable(bv, ci.ivarListAddress, ivarListType);
            defineSymbol(bv, ci.ivarListAddress, ci.name, "vl_");

            for (const auto& ii : ci.ivarList.ivars) {
                defineVariable(bv, ii.address, ivarType);
                defineSymbol(bv, ii.address, ii.name, "iv_");
            }
        }
        if (ci.metaClassInfo) {
            for (const auto& mi : ci.metaClassInfo->info.methodList.methods) {
                ++totalMethods;

                defineVariable(bv, mi.address, methodType);
                defineSymbol(bv, mi.address, sanitizeSelector(mi.selector), "mt_");
                defineVariable(bv, mi.typeAddress, stringType(mi.type.size()));

                defineReference(bv, ci.metaClassInfo->info.methodList.address, mi.address);
                defineReference(bv, mi.address, mi.nameAddress);
                defineReference(bv, mi.address, mi.typeAddress);
                defineReference(bv, mi.address, mi.implAddress);
                applyMethodType(bv, ci.metaClassInfo->info, methodSelfType, mi);
                classComponent->AddFunction(bv->GetAnalysisFunction(bv->GetDefaultPlatform(), mi.implAddress));
            }
        }

        // Create a data variable and symbol for the method list header.
        defineVariable(bv, ci.methodListAddress, methodListType);
        defineSymbol(bv, ci.methodListAddress, ci.name, "ml_");
    }

    for (const auto classRef : info->classRefs) {
        bv->DefineDataVariable(classRef.address, taggedPointerType);

        if (classRef.referencedAddress != 0) {
            auto localClass = addressToClassMap.find(classRef.referencedAddress);
            if (localClass != addressToClassMap.end())
                defineSymbol(bv, classRef.address, localClass->second, "cr_");
        }
    }

    for (const auto superRef : info->superRefs) {
        bv->DefineDataVariable(superRef.address, taggedPointerType);

        if (superRef.referencedAddress == 0)
            continue;

        auto localClass = addressToClassMap.find(superRef.referencedAddress);
        if (localClass != addressToClassMap.end())
            defineSymbol(bv, superRef.address, localClass->second, "su_");
    }

    bv->CommitUndoActions();
    bv->UpdateAnalysis();

    auto elapsed = Performance::elapsed<std::chrono::milliseconds>(start);

    const auto log = BinaryNinja::LogRegistry::GetLogger(PluginLoggerName);
    log->LogInfo("Analysis results applied in %lu ms", elapsed.count());
    log->LogInfo("Found %d classes, %d methods, %d selector references",
        info->classes.size(), totalMethods, info->selectorRefs.size());
    log->LogInfo("Found %d CFString instances", info->cfStrings.size());
    log->LogInfo("Found %d class references, %d superclass references", info->classRefs.size(), info->superRefs.size());
}
