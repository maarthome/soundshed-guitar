// Tests for picking one plugin out of a multi-plugin LV2 bundle.
//
// A bundle such as ToobAmp.lv2 holds a dozen plugins, one .ttl each. Reducing a
// picked ToobFlanger.ttl to the bundle root used to load whichever plugin the
// scan returned first (Toob Cab IR). These tests pin how the picked .ttl is
// turned back into the plugin URI the loader selects by.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "resources/Lv2PluginSelector.h"

using guitarfx::pluginpath::Lv2PluginUriForPath;
using guitarfx::pluginpath::Lv2PluginUrisDeclaredIn;
using guitarfx::pluginpath::Lv2PluginUrisLinkedTo;
using guitarfx::pluginpath::ResolvePluginResourcePath;

namespace
{
int gFailures = 0;

void Expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "  FAILED: " << what << "\n";
        ++gFailures;
    }
}

void ExpectText(const std::string& actual, const std::string& expected, const std::string& what)
{
    if (actual != expected)
    {
        std::cerr << "  FAILED: " << what << "\n    expected: " << expected << "\n    actual:   " << actual << "\n";
        ++gFailures;
    }
}

constexpr const char* kManifest = R"TTL(
@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .
@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .
@prefix toob: <http://two-play.com/plugins/> .

# Cab IR first, as the bundle scan returns it.
<http://two-play.com/plugins/toob-cab-ir>
    a lv2:Plugin ;
    lv2:binary <ToobAmp.so> ;
    rdfs:seeAlso <ToobCabIR.ttl> .

toob:toob-flanger a lv2:Plugin ;
    lv2:binary <ToobAmp.so> ;
    rdfs:seeAlso <ToobFlanger.ttl>, <ui/ToobFlangerUi.ttl> .

<http://two-play.com/plugins/toob-chorus>
    a lv2:Plugin;lv2:binary <ToobAmp.so>;rdfs:seeAlso <./ToobChorus.ttl>.
)TTL";

constexpr const char* kFlangerTtl = R"TTL(
@prefix lv2: <http://lv2plug.in/ns/lv2core#> .
@prefix doap: <http://usefulinc.com/ns/doap#> .
@prefix urid: <http://lv2plug.in/ns/ext/urid#> .

<http://two-play.com/plugins/toob-flanger>
    a lv2:Plugin, lv2:FlangerPlugin ;
    doap:name "TooB Flanger ; with a . and a # in it" ;
    doap:description """Multi-line
       text. <http://not-a-subject> a lv2:Plugin .""" ;
    lv2:minorVersion 1 ; lv2:microVersion 0.5 ;
    lv2:port [
        a lv2:InputPort ;
        lv2:index 0 ;
        lv2:symbol "rate"
    ] .
)TTL";

void TestManifestLinksTtlToPlugin()
{
    const auto flanger = Lv2PluginUrisLinkedTo(kManifest, "ToobFlanger.ttl");
    Expect(flanger.size() == 1, "the manifest links ToobFlanger.ttl to one plugin");
    ExpectText(flanger.empty() ? "" : flanger.front(), "http://two-play.com/plugins/toob-flanger",
               "a prefixed subject expands to the full URI");

    const auto cab = Lv2PluginUrisLinkedTo(kManifest, "ToobCabIR.ttl");
    ExpectText(cab.empty() ? "" : cab.front(), "http://two-play.com/plugins/toob-cab-ir",
               "a bracketed subject is read as written");

    const auto chorus = Lv2PluginUrisLinkedTo(kManifest, "ToobChorus.ttl");
    ExpectText(chorus.empty() ? "" : chorus.front(), "http://two-play.com/plugins/toob-chorus",
               "statements without spaces around punctuation and a './' prefix still match");

    const auto ui = Lv2PluginUrisLinkedTo(kManifest, "ui/ToobFlangerUi.ttl");
    ExpectText(ui.empty() ? "" : ui.front(), "http://two-play.com/plugins/toob-flanger",
               "a second seeAlso in a subfolder belongs to the same plugin");

    Expect(Lv2PluginUrisLinkedTo(kManifest, "Missing.ttl").empty(), "an unlinked file names no plugin");
}

void TestTtlDeclaresPlugin()
{
    const auto uris = Lv2PluginUrisDeclaredIn(kFlangerTtl);
    Expect(uris.size() == 1, "the flanger .ttl declares exactly one plugin (literals and ports ignored)");
    ExpectText(uris.empty() ? "" : uris.front(), "http://two-play.com/plugins/toob-flanger",
               "the declared plugin is the flanger");

    const auto subclassOnly = Lv2PluginUrisDeclaredIn("@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n"
                                                      "<urn:example:verb> a lv2:ReverbPlugin .\n");
    Expect(subclassOnly.size() == 1, "a plugin typed only by its lv2 class is still a plugin");
}

void WriteFile(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

void TestPathResolution()
{
    const auto root = std::filesystem::temp_directory_path() / "soundshed-lv2-selector-test";
    std::filesystem::remove_all(root);
    const auto bundle = root / "ToobAmp.lv2";
    WriteFile(bundle / "manifest.ttl", kManifest);
    WriteFile(bundle / "ToobFlanger.ttl", kFlangerTtl);
    WriteFile(bundle / "ToobCabIR.ttl", "# described in the manifest only\n");
    WriteFile(bundle / "ToobAmp.so", "");
    WriteFile(root / "Loose.ttl", kFlangerTtl);

    ExpectText(Lv2PluginUriForPath(bundle / "ToobFlanger.ttl"), "http://two-play.com/plugins/toob-flanger",
               "a picked plugin .ttl names its plugin");
    ExpectText(Lv2PluginUriForPath(bundle / "ToobCabIR.ttl"), "http://two-play.com/plugins/toob-cab-ir",
               "the manifest answers for a .ttl that declares nothing itself");
    ExpectText(Lv2PluginUriForPath(bundle / "manifest.ttl"), "", "manifest.ttl names no single plugin");
    ExpectText(Lv2PluginUriForPath(bundle / "ToobAmp.so"), "", "the binary names no single plugin");
    ExpectText(Lv2PluginUriForPath(bundle), "", "the bundle root names no single plugin");
    ExpectText(Lv2PluginUriForPath(root / "Loose.ttl"), "", "a .ttl outside any bundle is not a selection");

    Expect(ResolvePluginResourcePath(bundle / "ToobFlanger.ttl") == (bundle / "ToobFlanger.ttl").lexically_normal(),
           "a plugin .ttl is stored as picked");
    Expect(ResolvePluginResourcePath(bundle / "manifest.ttl") == bundle, "manifest.ttl is stored as the bundle");
    Expect(ResolvePluginResourcePath(bundle / "ToobAmp.so") == bundle, "the binary is stored as the bundle");

    std::filesystem::remove_all(root);
}
} // namespace

int main()
{
    const auto runTest = [](void (*test)(), const char* label) {
        const int before = gFailures;
        test();
        std::cout << label << (gFailures == before ? " PASS" : " FAIL") << std::endl;
    };

    runTest(TestManifestLinksTtlToPlugin, "Lv2PluginSelector reads manifest links:");
    runTest(TestTtlDeclaresPlugin, "Lv2PluginSelector reads plugin declarations:");
    runTest(TestPathResolution, "Lv2PluginSelector resolves picked paths:");

    return gFailures == 0 ? 0 : 1;
}
