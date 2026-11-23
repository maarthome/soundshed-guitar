#include "resources/Lv2PluginSelector.h"

#include "resources/PluginPathUtils.h"
#include "util/PathEncoding.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>

namespace guitarfx::pluginpath
{
namespace
{
constexpr std::string_view kRdfType = "http://www.w3.org/1999/02/22-rdf-syntax-ns#type";
constexpr std::string_view kRdfsSeeAlso = "http://www.w3.org/2000/01/rdf-schema#seeAlso";
constexpr std::string_view kLv2Core = "http://lv2plug.in/ns/lv2core#";

/// Enough of Turtle to read an LV2 manifest: IRIs, prefixed names, literals
/// (skipped over, never interpreted), comments and the statement punctuation.
struct Token
{
    enum class Kind
    {
        Iri,
        Word,
        Literal,
        Punct
    };

    Kind kind;
    std::string text;
};

bool IsPunct(char ch)
{
    return ch == '.' || ch == ';' || ch == ',' || ch == '[' || ch == ']' || ch == '(' || ch == ')';
}

bool EndsWord(char ch)
{
    return std::isspace(static_cast<unsigned char>(ch)) != 0 || ch == '<' || ch == '"' || ch == '\'' || ch == '#' ||
           (IsPunct(ch) && ch != '.');
}

std::vector<Token> Tokenize(std::string_view text)
{
    std::vector<Token> tokens;
    std::size_t i = 0;

    while (i < text.size())
    {
        const char ch = text[i];

        if (std::isspace(static_cast<unsigned char>(ch)) != 0)
        {
            ++i;
        }
        else if (ch == '#')
        {
            while (i < text.size() && text[i] != '\n')
            {
                ++i;
            }
        }
        else if (ch == '<')
        {
            const auto close = text.find('>', i + 1);
            const auto end = close == std::string_view::npos ? text.size() : close;
            tokens.push_back({Token::Kind::Iri, std::string{text.substr(i + 1, end - i - 1)}});
            i = end + 1;
        }
        else if (ch == '"' || ch == '\'')
        {
            const bool triple = text.substr(i, 3) == std::string(3, ch);
            const std::string_view quote = triple ? text.substr(i, 3) : text.substr(i, 1);
            i += quote.size();

            while (i < text.size() && text.substr(i, quote.size()) != quote)
            {
                i += text[i] == '\\' ? 2 : 1;
            }

            i += quote.size();
            tokens.push_back({Token::Kind::Literal, {}});
        }
        else if (IsPunct(ch))
        {
            tokens.push_back({Token::Kind::Punct, std::string(1, ch)});
            ++i;
        }
        else
        {
            const std::size_t start = i;

            // A '.' belongs to the word unless it ends it: "lv2:Plugin." closes a statement,
            // "1.0" and "doap:foo.bar" do not.
            while (i < text.size() && !EndsWord(text[i]) &&
                   !(text[i] == '.' && (i + 1 >= text.size() || EndsWord(text[i + 1]))))
            {
                ++i;
            }

            if (i == start)
            {
                tokens.push_back({Token::Kind::Punct, std::string(1, text[i])});
                ++i;
            }
            else
            {
                tokens.push_back({Token::Kind::Word, std::string{text.substr(start, i - start)}});
            }
        }
    }

    return tokens;
}

/// One subject with each predicate's objects, expanded to full IRIs where possible.
struct Subject
{
    std::string iri;
    std::multimap<std::string, std::string> properties;
};

class TurtleReader
{
  public:
    explicit TurtleReader(std::string_view text) : mTokens(Tokenize(text))
    {
    }

    std::vector<Subject> Read()
    {
        std::vector<Subject> subjects;

        while (mPos < mTokens.size())
        {
            const std::size_t start = mPos;

            if (ReadDirective())
            {
                continue;
            }

            std::vector<Token> statement = TakeStatement();

            if (auto subject = ParseStatement(statement); !subject.iri.empty())
            {
                subjects.push_back(std::move(subject));
            }

            if (mPos == start)
            {
                ++mPos;
            }
        }

        return subjects;
    }

  private:
    bool ReadDirective()
    {
        const auto& first = mTokens[mPos];

        if (first.kind != Token::Kind::Word)
        {
            return false;
        }

        std::string lower = first.text;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        const bool sparqlStyle = lower == "prefix" || lower == "base";
        const bool turtleStyle = lower == "@prefix" || lower == "@base";

        if (!sparqlStyle && !turtleStyle)
        {
            return false;
        }

        const bool isPrefix = lower == "prefix" || lower == "@prefix";
        const std::size_t argCount = isPrefix ? 2 : 1;

        if (mPos + argCount >= mTokens.size())
        {
            mPos = mTokens.size();
            return true;
        }

        if (isPrefix)
        {
            std::string name = mTokens[mPos + 1].text;

            if (!name.empty() && name.back() == ':')
            {
                name.pop_back();
            }

            mPrefixes[name] = mTokens[mPos + 2].text;
        }

        mPos += argCount + 1;

        if (turtleStyle && mPos < mTokens.size() && mTokens[mPos].kind == Token::Kind::Punct &&
            mTokens[mPos].text == ".")
        {
            ++mPos;
        }

        return true;
    }

    std::vector<Token> TakeStatement()
    {
        std::vector<Token> statement;
        int depth = 0;

        while (mPos < mTokens.size())
        {
            const auto& token = mTokens[mPos++];

            if (token.kind == Token::Kind::Punct)
            {
                if (token.text == "[" || token.text == "(")
                {
                    ++depth;
                }
                else if (token.text == "]" || token.text == ")")
                {
                    depth = std::max(0, depth - 1);
                }
                else if (token.text == "." && depth == 0)
                {
                    break;
                }
            }

            statement.push_back(token);
        }

        return statement;
    }

    std::string Expand(const Token& token) const
    {
        if (token.kind == Token::Kind::Iri)
        {
            return token.text;
        }

        if (token.kind != Token::Kind::Word)
        {
            return {};
        }

        if (token.text == "a")
        {
            return std::string{kRdfType};
        }

        const auto colon = token.text.find(':');

        if (colon == std::string::npos)
        {
            return {};
        }

        const auto it = mPrefixes.find(token.text.substr(0, colon));
        return it != mPrefixes.end() ? it->second + token.text.substr(colon + 1) : token.text;
    }

    Subject ParseStatement(const std::vector<Token>& statement) const
    {
        Subject subject;

        // Blank-node subjects describe ports and the like, never a plugin.
        if (statement.empty() || statement.front().kind == Token::Kind::Punct)
        {
            return subject;
        }

        subject.iri = Expand(statement.front());
        std::size_t i = 1;

        while (i < statement.size())
        {
            const std::string predicate = Expand(statement[i++]);
            bool expectObject = true;
            int depth = 0;

            for (; i < statement.size(); ++i)
            {
                const auto& token = statement[i];

                if (token.kind == Token::Kind::Punct)
                {
                    if (token.text == "[" || token.text == "(")
                    {
                        ++depth;
                    }
                    else if (token.text == "]" || token.text == ")")
                    {
                        depth = std::max(0, depth - 1);
                    }
                    else if (depth == 0 && token.text == ",")
                    {
                        expectObject = true;
                        continue;
                    }
                    else if (depth == 0 && token.text == ";")
                    {
                        ++i;
                        break;
                    }

                    expectObject = false;
                    continue;
                }

                if (depth == 0 && expectObject && !predicate.empty())
                {
                    subject.properties.emplace(predicate, Expand(token));
                }

                expectObject = false;
            }
        }

        return subject;
    }

    std::vector<Token> mTokens;
    std::size_t mPos = 0;
    std::map<std::string, std::string> mPrefixes;
};

bool IsLv2PluginClass(const std::string& iri, bool allowSubclasses)
{
    if (iri.size() <= kLv2Core.size() || iri.compare(0, kLv2Core.size(), kLv2Core) != 0)
    {
        return false;
    }

    const std::string_view local = std::string_view{iri}.substr(kLv2Core.size());

    if (local == "Plugin")
    {
        return true;
    }

    return allowSubclasses && local.size() > 6 && local.substr(local.size() - 6) == "Plugin";
}

bool IsPlugin(const Subject& subject, bool allowSubclasses)
{
    const auto [begin, end] = subject.properties.equal_range(std::string{kRdfType});
    return std::any_of(
        begin, end, [allowSubclasses](const auto& entry) { return IsLv2PluginClass(entry.second, allowSubclasses); });
}

/// A plugin URI is absolute; a relative subject would need the bundle's own URI to resolve.
bool IsAbsoluteIri(const std::string& iri)
{
    const auto colon = iri.find(':');
    return colon != std::string::npos && colon > 0 && iri.find('/') > colon;
}

std::string NormalizeRelative(std::string_view path)
{
    std::string text{path};
    std::replace(text.begin(), text.end(), '\\', '/');

    while (text.rfind("./", 0) == 0)
    {
        text.erase(0, 2);
    }

    return text;
}

void AddUnique(std::vector<std::string>& values, const std::string& value)
{
    if (std::find(values.begin(), values.end(), value) == values.end())
    {
        values.push_back(value);
    }
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);

    if (!stream)
    {
        return {};
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

std::string ToLower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return text;
}
} // namespace

std::vector<std::string> Lv2PluginUrisLinkedTo(std::string_view manifestText, std::string_view ttlRelativePath)
{
    const std::string wanted = NormalizeRelative(ttlRelativePath);
    std::vector<std::string> uris;

    for (const auto& subject : TurtleReader(manifestText).Read())
    {
        if (!IsAbsoluteIri(subject.iri) || !IsPlugin(subject, /*allowSubclasses=*/false))
        {
            continue;
        }

        const auto [begin, end] = subject.properties.equal_range(std::string{kRdfsSeeAlso});

        for (auto it = begin; it != end; ++it)
        {
            if (NormalizeRelative(it->second) == wanted)
            {
                AddUnique(uris, subject.iri);
            }
        }
    }

    return uris;
}

std::vector<std::string> Lv2PluginUrisDeclaredIn(std::string_view ttlText)
{
    std::vector<std::string> uris;

    for (const auto& subject : TurtleReader(ttlText).Read())
    {
        if (IsAbsoluteIri(subject.iri) && IsPlugin(subject, /*allowSubclasses=*/true))
        {
            AddUnique(uris, subject.iri);
        }
    }

    return uris;
}

std::string Lv2PluginUriForPath(const std::filesystem::path& path)
{
    if (path.empty() || ToLower(util::PathToUtf8(path.extension())) != ".ttl" ||
        ToLower(util::PathToUtf8(path.filename())) == "manifest.ttl")
    {
        return {};
    }

    const auto bundle = ResolvePluginBundlePath(path);

    if (bundle == path || PluginFormatFromPath(bundle) != PluginFormat::LV2)
    {
        return {};
    }

    std::error_code ec;
    const auto relative = std::filesystem::relative(path, bundle, ec);

    if (ec || relative.empty())
    {
        return {};
    }

    // The manifest is what lilv reads first, and it says which plugin a file describes even
    // when that file uses prefixes or classes this reader does not follow.
    auto uris = Lv2PluginUrisLinkedTo(ReadText(bundle / "manifest.ttl"), util::PathToUtf8(relative));

    if (uris.size() != 1)
    {
        uris = Lv2PluginUrisDeclaredIn(ReadText(path));
    }

    return uris.size() == 1 ? uris.front() : std::string{};
}

std::filesystem::path ResolvePluginResourcePath(const std::filesystem::path& path)
{
    if (!Lv2PluginUriForPath(path).empty())
    {
        return path.lexically_normal();
    }

    return ResolvePluginBundlePath(path);
}
} // namespace guitarfx::pluginpath
