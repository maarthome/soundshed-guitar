#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace guitarfx::pluginpath
{
/// Picking one plugin out of an LV2 bundle that holds several.
///
/// A bundle such as ToobAmp.lv2 carries one .ttl per plugin next to its
/// manifest.ttl. Scanning the bundle yields every plugin in it, so a path that
/// has been reduced to the bundle root can only ever load the first one. When
/// the user picks a plugin's own .ttl, that file is what says which plugin they
/// meant; these helpers read the plugin URI it describes, so the loader can
/// select it and the resource library can keep the plugins apart.

/// URIs of the plugins the manifest links to `ttlRelativePath` (relative to the
/// bundle root, '/'-separated) through rdfs:seeAlso.
[[nodiscard]] std::vector<std::string> Lv2PluginUrisLinkedTo(std::string_view manifestText,
                                                             std::string_view ttlRelativePath);

/// URIs of the subjects a Turtle document declares as LV2 plugins (typed
/// lv2:Plugin, or one of the lv2core plugin classes such as lv2:ReverbPlugin).
[[nodiscard]] std::vector<std::string> Lv2PluginUrisDeclaredIn(std::string_view ttlText);

/// The plugin URI named by `path` when it is a plugin's own .ttl inside an
/// .lv2 bundle: not manifest.ttl, and naming exactly one plugin. Empty for
/// every other path, including a bundle root. Reads the files.
[[nodiscard]] std::string Lv2PluginUriForPath(const std::filesystem::path& path);

/// The path a plugin resource is stored under: a plugin-specific LV2 .ttl is
/// kept as picked (it is the only record of which plugin was chosen); every
/// other selection is normalized to its bundle root.
[[nodiscard]] std::filesystem::path ResolvePluginResourcePath(const std::filesystem::path& path);
} // namespace guitarfx::pluginpath
