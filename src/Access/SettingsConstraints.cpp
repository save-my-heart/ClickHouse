#include <Access/SettingsConstraints.h>
#include <Access/SettingsProfileElement.h>
#include <Access/resolveSetting.h>
#include <Access/AccessControl.h>
#include <Core/Settings.h>
#include <Storages/MergeTree/MergeTreeSettings.h>
#include <Common/FieldVisitorToString.h>
#include <Common/FieldAccurateComparison.h>
#include <Common/SettingSource.h>
#include <IO/WriteHelpers.h>

#include <string_view>
#include <unordered_map>

namespace DB
{
namespace Setting
{
    extern const SettingsBool allow_ddl;
    extern const SettingsUInt64 readonly;
}

namespace ErrorCodes
{
    extern const int READONLY;
    extern const int QUERY_IS_PROHIBITED;
    extern const int SETTING_CONSTRAINT_VIOLATION;
    extern const int UNKNOWN_SETTING;
}

namespace
{
struct SettingSourceRestrictions
{
    constexpr SettingSourceRestrictions() { allowed_sources.set(); }

    constexpr SettingSourceRestrictions(std::initializer_list<SettingSource> allowed_sources_)
    {
        for (auto allowed_source : allowed_sources_)
            setSourceAllowed(allowed_source, true);
    }

    constexpr bool isSourceAllowed(SettingSource source) { return allowed_sources[source]; }
    constexpr void setSourceAllowed(SettingSource source, bool allowed) { allowed_sources[source] = allowed; }

    std::bitset<SettingSource::COUNT> allowed_sources;
};

const std::unordered_map<std::string_view, SettingSourceRestrictions> SETTINGS_SOURCE_RESTRICTIONS = {
    {"max_sessions_for_user", {SettingSource::PROFILE}},
};

SettingSourceRestrictions getSettingSourceRestrictions(std::string_view name)
{
    auto settingConstraintIter = SETTINGS_SOURCE_RESTRICTIONS.find(name);
    if (settingConstraintIter != SETTINGS_SOURCE_RESTRICTIONS.end())
        return settingConstraintIter->second;
    return SettingSourceRestrictions(); // allows everything
}

}

SettingsConstraints::SettingsConstraints(const AccessControl & access_control_) : access_control(&access_control_)
{
}

SettingsConstraints::SettingsConstraints(const SettingsConstraints & src) = default;
SettingsConstraints & SettingsConstraints::operator=(const SettingsConstraints & src) = default;
SettingsConstraints::SettingsConstraints(SettingsConstraints && src) noexcept = default;
SettingsConstraints & SettingsConstraints::operator=(SettingsConstraints && src) noexcept = default;
SettingsConstraints::~SettingsConstraints() = default;


void SettingsConstraints::clear()
{
    constraints.clear();
    settings_alias_cache.clear();
}

void SettingsConstraints::set(const String & full_name, const Field & min_value, const Field & max_value, const std::vector<Field> & disallowed_values, SettingConstraintWritability writability)
{
    std::string resolved_name{resolveSettingName(full_name)};

    auto & constraint = constraints[resolved_name];

    if (full_name != resolved_name)
        settings_alias_cache[full_name] = resolved_name;

    if (!min_value.isNull())
        constraint.min_value = settingCastValueUtil(resolved_name, min_value);
    if (!max_value.isNull())
        constraint.max_value = settingCastValueUtil(resolved_name, max_value);
    if (!disallowed_values.empty())
    {
        for (const auto & allowed_value : disallowed_values)
            constraint.disallowed_values.emplace_back(settingCastValueUtil(resolved_name, allowed_value));
    }
    constraint.writability = writability;
}

void SettingsConstraints::get(const Settings & current_settings, std::string_view short_name, Field & min_value, Field & max_value, std::vector<Field> & disallowed_values, SettingConstraintWritability & writability) const
{
    // NOTE: for `Settings` short name is equal to full name
    auto checker = getChecker(current_settings, short_name);
    min_value = checker.constraint.min_value;
    max_value = checker.constraint.max_value;
    disallowed_values = checker.constraint.disallowed_values;
    writability = checker.constraint.writability;
}

void SettingsConstraints::get(const MergeTreeSettings &, std::string_view short_name, Field & min_value, Field & max_value, std::vector<Field> & disallowed_values, SettingConstraintWritability & writability) const
{
    auto checker = getMergeTreeChecker(short_name);
    min_value = checker.constraint.min_value;
    max_value = checker.constraint.max_value;
    disallowed_values = checker.constraint.disallowed_values;
    writability = checker.constraint.writability;
}

void SettingsConstraints::merge(const SettingsConstraints & other)
{
    if (access_control->doesSettingsConstraintsReplacePrevious())
    {
        for (const auto & [other_name, other_constraint] : other.constraints)
        {
            constraints[other_name] = other_constraint;
        }
    }
    else
    {
        for (const auto & [other_name, other_constraint] : other.constraints)
        {
            auto & constraint = constraints[other_name];
            if (!other_constraint.min_value.isNull())
                constraint.min_value = other_constraint.min_value;
            if (!other_constraint.max_value.isNull())
                constraint.max_value = other_constraint.max_value;
            if (!other_constraint.disallowed_values.empty())
                constraint.disallowed_values = other_constraint.disallowed_values;
            if (other_constraint.writability == SettingConstraintWritability::CONST)
                constraint.writability = SettingConstraintWritability::CONST; // NOTE: In this mode <readonly/> flag cannot be overridden to be false
        }
    }

    for (const auto & [other_alias, other_resolved_name] : settings_alias_cache)
        settings_alias_cache.try_emplace(other_alias, other_resolved_name);
}


void SettingsConstraints::check(const Settings & current_settings, const AlterSettingsProfileElements & profile_elements, SettingSource source) const
{
    check(current_settings, profile_elements.add_settings, source);
    check(current_settings, profile_elements.modify_settings, source);
    /// We don't check `drop_settings` here.
}

void SettingsConstraints::check(const Settings & current_settings, const SettingsProfileElements & profile_elements, SettingSource source) const
{
    for (const auto & element : profile_elements)
    {
        if (SettingsProfileElements::isAllowBackupSetting(element.setting_name))
            continue;

        if (element.value)
        {
            SettingChange value(element.setting_name, *element.value);
            check(current_settings, value, source);
        }

        if (element.min_value)
        {
            SettingChange value(element.setting_name, *element.min_value);
            check(current_settings, value, source);
        }

        if (element.max_value)
        {
            SettingChange value(element.setting_name, *element.max_value);
            check(current_settings, value, source);
        }

        /// Don't check disallowed_values here in the profile elements because they make constrains more restrictive
        /// and don't allow to bypass constraints from config by creating a user with custom constraints. The check
        /// for disallowed values are instead implemented in SettingsConstraints::Checker::check.

        SettingConstraintWritability new_value = SettingConstraintWritability::WRITABLE;
        SettingConstraintWritability old_value = SettingConstraintWritability::WRITABLE;

        if (element.writability)
            new_value = *element.writability;

        auto setting_name = Settings::resolveName(element.setting_name);
        auto it = constraints.find(setting_name);
        if (it != constraints.end())
            old_value = it->second.writability;

        if (new_value != old_value)
        {
            if (old_value == SettingConstraintWritability::CONST)
                throw Exception(ErrorCodes::SETTING_CONSTRAINT_VIOLATION, "Setting {} should not be changed", setting_name);
        }
    }
}

void SettingsConstraints::check(const Settings & current_settings, const SettingChange & change, SettingSource source) const
{
    checkImpl(current_settings, const_cast<SettingChange &>(change), THROW_ON_VIOLATION, source);
}

void SettingsConstraints::check(const Settings & current_settings, const SettingsChanges & changes, SettingSource source) const
{
    for (const auto & change : changes)
        check(current_settings, change, source);
}

void SettingsConstraints::check(const Settings & current_settings, SettingsChanges & changes, SettingSource source) const
{
    std::erase_if(
        changes,
        [&](SettingChange & change) -> bool
        {
            return !checkImpl(current_settings, change, THROW_ON_VIOLATION, source);
        });
}

void SettingsConstraints::check(const MergeTreeSettings & current_settings, const SettingChange & change) const
{
    checkImpl(current_settings, const_cast<SettingChange &>(change), THROW_ON_VIOLATION);
}

void SettingsConstraints::check(const MergeTreeSettings & current_settings, const SettingsChanges & changes) const
{
    for (const auto & change : changes)
        check(current_settings, change);
}

void SettingsConstraints::clamp(const Settings & current_settings, SettingsChanges & changes, SettingSource source) const
{
    std::erase_if(
        changes,
        [&](SettingChange & change) -> bool
        {
            return !checkImpl(current_settings, change, CLAMP_ON_VIOLATION, source);
        });
}

template <typename SettingsT>
bool getNewValueToCheck(const SettingsT & current_settings, SettingChange & change, Field & new_value, bool throw_on_failure)
{
    Field current_value;
    bool has_current_value = current_settings.tryGet(change.name, current_value);

    /// Setting isn't checked if value has not changed.
    if (has_current_value && change.value == current_value)
        return false;

    if (throw_on_failure)
        new_value = SettingsT::castValueUtil(change.name, change.value);
    else
    {
        try
        {
            new_value = SettingsT::castValueUtil(change.name, change.value);
        }
        catch (...)
        {
            return false;
        }
    }

    /// Setting isn't checked if value has not changed.
    if (has_current_value && new_value == current_value)
        return false;

    return true;
}

bool SettingsConstraints::checkImpl(const Settings & current_settings,
                                    SettingChange & change,
                                    ReactionOnViolation reaction,
                                    SettingSource source) const
{
    std::string_view setting_name = Settings::resolveName(change.name);

    if (setting_name == "profile")
        return true;

    if (reaction == THROW_ON_VIOLATION)
    {
        try
        {
            access_control->checkSettingNameIsAllowed(setting_name);
        }
        catch (Exception & e)
        {
            if (e.code() == ErrorCodes::UNKNOWN_SETTING)
            {
                if (const auto hints = current_settings.getHints(change.name); !hints.empty())
                {
                    e.addMessage(fmt::format("Maybe you meant {}", toString(hints)));
                }
            }
            throw;
        }
    }
    else if (!access_control->isSettingNameAllowed(setting_name))
        return false;

    Field new_value;
    if (!getNewValueToCheck(current_settings, change, new_value, reaction == THROW_ON_VIOLATION))
        return false;

    return getChecker(current_settings, setting_name).check(change, new_value, reaction, source);
}

bool SettingsConstraints::checkImpl(const MergeTreeSettings & current_settings, SettingChange & change, ReactionOnViolation reaction) const
{
    Field new_value;
    if (!getNewValueToCheck(current_settings, change, new_value, reaction == THROW_ON_VIOLATION))
        return false;
    return getMergeTreeChecker(change.name).check(change, new_value, reaction, SettingSource::QUERY);
}

bool SettingsConstraints::Checker::check(SettingChange & change,
                                         const Field & new_value,
                                         ReactionOnViolation reaction,
                                         SettingSource source) const
{
    if (!explain.text.empty())
    {
        if (reaction == THROW_ON_VIOLATION)
            throw Exception(explain, code);
        return false;
    }

    std::string_view setting_name = setting_name_resolver(change.name);

    auto less_or_cannot_compare = [=](const Field & left, const Field & right)
    {
        if (reaction == THROW_ON_VIOLATION)
            return accurateLess(left, right);
        try
        {
            return accurateLess(left, right);
        }
        catch (...)
        {
            return true;
        }
    };

    auto equals_or_cannot_compare = [=](const Field & left, const Field & right)
    {
        if (reaction == THROW_ON_VIOLATION)
            return accurateEquals(left, right);
        try
        {
            return accurateEquals(left, right);
        }
        catch (...)
        {
            return true;
        }
    };


    if (constraint.writability == SettingConstraintWritability::CONST)
    {
        if (reaction == THROW_ON_VIOLATION)
            throw Exception(ErrorCodes::SETTING_CONSTRAINT_VIOLATION, "Setting {} should not be changed", setting_name);
        return false;
    }

    const auto & min_value = constraint.min_value;
    const auto & max_value = constraint.max_value;
    const auto & disallowed_values = constraint.disallowed_values;

    if (!min_value.isNull() && !max_value.isNull() && less_or_cannot_compare(max_value, min_value))
    {
        if (reaction == THROW_ON_VIOLATION)
            throw Exception(
                ErrorCodes::SETTING_CONSTRAINT_VIOLATION,
                "The maximum ({}) value is less than the minimum ({}) value for setting {}",
                max_value,
                min_value,
                setting_name);
        return false;
    }

    if (!min_value.isNull() && less_or_cannot_compare(new_value, min_value))
    {
        if (reaction == THROW_ON_VIOLATION)
        {
            throw Exception(ErrorCodes::SETTING_CONSTRAINT_VIOLATION, "Setting {} shouldn't be less than {}",
                setting_name, applyVisitor(FieldVisitorToString(), min_value));
        }
        change.value = min_value;
    }

    if (!max_value.isNull() && less_or_cannot_compare(max_value, new_value))
    {
        if (reaction == THROW_ON_VIOLATION)
        {
            throw Exception(ErrorCodes::SETTING_CONSTRAINT_VIOLATION, "Setting {} shouldn't be greater than {}",
                setting_name, applyVisitor(FieldVisitorToString(), max_value));
        }
        change.value = max_value;
    }

    for (const auto & value : disallowed_values)
    {
        bool equals = equals_or_cannot_compare(value, new_value);
        if (equals)
            throw Exception(ErrorCodes::SETTING_CONSTRAINT_VIOLATION, "Setting {} shouldn't be {}",
                setting_name, applyVisitor(FieldVisitorToString(), value));
    }

    if (!getSettingSourceRestrictions(setting_name).isSourceAllowed(source))
    {
        if (reaction == THROW_ON_VIOLATION)
            throw Exception(ErrorCodes::READONLY, "Setting {} is not allowed to be set by {}", setting_name, toString(source));
        return false;
    }

    return true;
}

std::string_view SettingsConstraints::resolveSettingNameWithCache(std::string_view name) const
{
    if (auto it = settings_alias_cache.find(name); it != settings_alias_cache.end())
        return it->second;
    return name;
}

SettingsConstraints::Checker SettingsConstraints::getChecker(const Settings & current_settings, std::string_view setting_name) const
{
    auto resolved_name = resolveSettingNameWithCache(setting_name);
    if (!current_settings[Setting::allow_ddl] && resolved_name == "allow_ddl")
        return Checker(PreformattedMessage::create("Cannot modify 'allow_ddl' setting when DDL queries are prohibited for the user"),
                       ErrorCodes::QUERY_IS_PROHIBITED);

    /** The `readonly` value is understood as follows:
      * 0 - no read-only restrictions.
      * 1 - only read requests, as well as changing settings with `changeable_in_readonly` flag.
      * 2 - only read requests, as well as changing settings, except for the `readonly` setting.
      */

    if (current_settings[Setting::readonly] > 1 && resolved_name == "readonly")
        return Checker(PreformattedMessage::create("Cannot modify 'readonly' setting in readonly mode"), ErrorCodes::READONLY);

    if (access_control)
    {
        bool allowed_experimental = access_control->getAllowExperimentalTierSettings();
        bool allowed_beta = access_control->getAllowBetaTierSettings();
        if (!allowed_experimental || !allowed_beta)
        {
            auto setting_tier = current_settings.getTier(resolved_name);
            if (setting_tier == SettingsTierType::EXPERIMENTAL && !allowed_experimental)
                return Checker(
                    PreformattedMessage::create(
                        "Cannot modify setting '{}'. Changes to EXPERIMENTAL settings are disabled in the server config ('allow_feature_tier')",
                        setting_name),
                    ErrorCodes::READONLY);
            if (setting_tier == SettingsTierType::BETA && !allowed_beta)
                return Checker(
                    PreformattedMessage::create(
                        "Cannot modify setting '{}'. Changes to BETA settings are disabled in the server config ('allow_feature_tier')",
                        setting_name),
                    ErrorCodes::READONLY);
        }
    }

    auto it = constraints.find(resolved_name);
    if (current_settings[Setting::readonly] == 1)
    {
        if (it == constraints.end() || it->second.writability != SettingConstraintWritability::CHANGEABLE_IN_READONLY)
            return Checker(PreformattedMessage::create("Cannot modify '{}' setting in readonly mode", setting_name),
                           ErrorCodes::READONLY);
    }
    else // For both readonly=0 and readonly=2
    {
        if (it == constraints.end())
            return Checker(Settings::resolveName); // Allowed
    }
    return Checker(it->second, Settings::resolveName);
}

SettingsConstraints::Checker SettingsConstraints::getMergeTreeChecker(std::string_view short_name) const
{
    auto full_name = settingFullName<MergeTreeSettings>(short_name);
    auto it = constraints.find(resolveSettingNameWithCache(full_name));
    if (it == constraints.end())
        return Checker(MergeTreeSettings::resolveName); // Allowed
    return Checker(it->second, MergeTreeSettings::resolveName);
}

bool SettingsConstraints::Constraint::operator==(const Constraint & other) const
{
    return writability == other.writability && min_value == other.min_value && max_value == other.max_value && disallowed_values == other.disallowed_values;
}

bool operator ==(const SettingsConstraints & left, const SettingsConstraints & right)
{
    return left.constraints == right.constraints;
}
}
