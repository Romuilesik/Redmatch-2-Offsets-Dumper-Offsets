#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <ranges>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

struct FieldEntry {
    std::string name;
    std::string type;
    uint32_t    offset{};
    bool        is_static{};
};

struct MethodEntry {
    std::string name;
    std::string return_type;
    std::string params;
    uint64_t    rva{};
    uint64_t    offset{};
};

struct ClassEntry {
    std::string               ns;
    std::string               name;
    std::string               parent;
    std::vector<FieldEntry>   fields;
    std::vector<MethodEntry>  methods;

    [[nodiscard]] std::string full_name() const {
        return ns.empty() ? name : ns + "." + name;
    }
};

static bool is_obfuscated(std::string_view s) {
    for (unsigned char c : s)
        if (c > 0x7E && c != '_') return true;
    return false;
}

static std::string safe_ident(std::string_view s, bool upper = false) {
    if (is_obfuscated(s)) {
        // Hash the obfuscated name to a short hex suffix for uniqueness
        size_t h = 0;
        for (unsigned char c : s) h = h * 31 + c;
        return std::format("obf_{:08x}", static_cast<uint32_t>(h));
    }
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        out += (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
        ? (upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c)
        : '_';
    return out;
}

static std::string utc_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()) % 1'000'000;
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
        << "." << std::setw(6) << std::setfill('0') << us.count() << " UTC";
    return ss.str();
}

static std::string trim(std::string_view s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos) return {};
    auto e = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(b, e - b + 1));
}

static bool contains_word(std::string_view haystack, std::string_view needle) {
    auto pos = haystack.find(needle);
    if (pos == std::string_view::npos) return false;
    bool left_ok = (pos == 0) || !std::isalnum(static_cast<unsigned char>(haystack[pos - 1]));
    bool right_ok = (pos + needle.size() >= haystack.size())
        || !std::isalnum(static_cast<unsigned char>(haystack[pos + needle.size()]));
    return left_ok && right_ok;
}

std::vector<ClassEntry> parse_dump_cs(const fs::path& path) {
    std::ifstream file(path);
    if (!file) {
        std::cerr << std::format("[-] Cannot open: {}\n", path.string());
        return {};
    }

    static const std::regex re_namespace(R"(^//\s*Namespace:\s*(.*?)\s*$)");
    static const std::regex re_class_decl(R"(\bclass\s+(\S+?)(?:<[^>]*>)?(?:\s*:\s*(\S+?))?\s*(?://|$|\{))");
    static const std::regex re_field_inline(R"(^\s*((?:(?:public|private|protected|internal|static|readonly|new|volatile)\s+)+).+;\s*//\s*0x([0-9A-Fa-f]+)\s*$)");
    static const std::regex re_method_meta(
        R"(^\s*//\s*RVA:\s*0x([0-9A-Fa-f]+)\s+Offset:\s*0x([0-9A-Fa-f]+))");
    static const std::regex re_field_mods(R"(^\s*((?:(?:public|private|protected|internal|static|readonly|new|volatile)\s+)+))");
    static const std::regex re_method_decl(
        R"(^\s*((?:(?:public|private|protected|internal|static|virtual|override|abstract|new|extern)\s+)*)([\w\.<>\[\], \*]+?)\s+(\w+)\s*\(([^)]*)\)\s*[{;])");

    std::vector<ClassEntry>   classes;
    std::optional<ClassEntry> current;

    std::string pending_ns;
    bool        next_is_method = false;
    uint64_t    pending_rva = 0;
    uint64_t    pending_moff = 0;

    auto flush = [&] {
        if (current && (!current->fields.empty() || !current->methods.empty()))
            classes.push_back(std::move(*current));
        current.reset();
        };

    std::string line;
    while (std::getline(file, line)) {
        std::smatch m;

        if (std::regex_search(line, m, re_namespace)) {
            pending_ns = trim(m[1].str());
            next_is_method = false;
            continue;
        }

        if (std::regex_search(line, m, re_class_decl)) {
            flush();
            ClassEntry cls;
            cls.ns = pending_ns;
            cls.name = m[1].str();
            cls.parent = trim(m[2].str());
            auto strip_generic = [](std::string& s) {
                auto p = s.find('<');
                if (p != std::string::npos) s = s.substr(0, p);
                };
            strip_generic(cls.name);
            strip_generic(cls.parent);
            current = std::move(cls);
            next_is_method = false;
            continue;
        }

        if (!current) continue;

        {
            std::smatch fi_m;
            if (std::regex_search(line, fi_m, re_field_inline)) {
                std::string rest = line.substr(fi_m[1].length());
                auto semi = rest.find(';');
                if (semi != std::string::npos) {
                    rest = rest.substr(0, semi);
                    auto e = rest.find_last_not_of(" \t");
                    if (e != std::string::npos) rest = rest.substr(0, e + 1);
                    auto sp = rest.find_last_of(" \t");
                    if (sp != std::string::npos) {
                        FieldEntry f;
                        f.name = rest.substr(sp + 1);
                        f.type = trim(rest.substr(0, sp));
                        f.offset = static_cast<uint32_t>(std::stoul(fi_m[2].str(), nullptr, 16));
                        f.is_static = fi_m[1].str().find("static") != std::string::npos;
                        current->fields.push_back(std::move(f));
                    }
                }
                next_is_method = false;
                continue;
            }
        }

        if (std::regex_search(line, m, re_method_meta)) {
            next_is_method = true;
            pending_rva = std::stoull(m[1].str(), nullptr, 16);
            pending_moff = std::stoull(m[2].str(), nullptr, 16);
            continue;
        }


        if (next_is_method) {
            next_is_method = false;
            if (std::regex_search(line, m, re_method_decl)) {
                MethodEntry me;
                me.return_type = trim(m[2].str());
                me.name = trim(m[3].str());
                me.params = trim(m[4].str());
                me.rva = pending_rva;
                me.offset = pending_moff;
                current->methods.push_back(std::move(me));
            }
            continue;
        }
    }
    flush();
    return classes;
}

void write_hpp(const std::vector<ClassEntry>& classes, const fs::path& out, const std::string& ts) {
    std::ofstream f(out);
    f << std::format("// Generated using https://github.com/Romuilesik/Redmatch-2-Offsets-Dumper-Offsets\n// {}\n\n#pragma once\n#include <cstdint>\n\n", ts);

    for (auto& cls : classes) {
        if (cls.fields.empty() && cls.methods.empty()) continue;
        std::string ns = safe_ident(cls.full_name());
        f << std::format("namespace {} {{\n", ns);
        if (!cls.parent.empty())
            f << std::format("    // parent: {}\n", cls.parent);
        if (!cls.fields.empty()) {
            f << "    // Fields\n";
            for (auto& fld : cls.fields) {
                std::string ident = safe_ident(fld.name) + (fld.is_static ? "_static" : "");
                std::string obf_comment = is_obfuscated(fld.name) ? std::format(" [obf: {}]", fld.name) : "";
                f << std::format("    constexpr std::ptrdiff_t {} = {:#x}; // {}{}\n", ident, fld.offset, fld.type, obf_comment);
            }
        }
        if (!cls.methods.empty()) {
            f << "    // Methods\n";
            for (auto& me : cls.methods) {
                std::string ret = me.return_type.empty() ? "" : std::format(" -> {}", me.return_type);
                f << std::format("    // [RVA {:#010x}] {}({}){}\n", me.rva, me.name, me.params, ret);
            }
        }
        f << "}\n\n";
    }
    std::cout << std::format("[+] {}\n", out.string());
}

void write_json(const std::vector<ClassEntry>& classes, const fs::path& out, const std::string& ts) {
    std::ofstream f(out);

    auto esc = [](const std::string& s) {
        std::string r;
        for (char c : s) {
            if (c == '"')  r += "\\\"";
            else if (c == '\\') r += "\\\\";
            else r += c;
        }
        return r;
        };

    f << "{\n";
    f << "  \"generator\": \"https://github.com/Romuilesik/Redmatch-2-Offsets-Dumper-Offsets\",\n";
    f << std::format("  \"generated_at\": \"{}\",\n", ts);
    f << "  \"classes\": [\n";

    bool first_cls = true;
    for (auto& cls : classes) {
        if (cls.fields.empty() && cls.methods.empty()) continue;
        if (!first_cls) f << ",\n";
        first_cls = false;

        f << "    {\n";
        f << std::format("      \"namespace\": \"{}\",\n", esc(cls.ns));
        f << std::format("      \"name\": \"{}\",\n", esc(cls.name));
        f << std::format("      \"full_name\": \"{}\",\n", esc(cls.full_name()));
        f << std::format("      \"parent\": \"{}\",\n", esc(cls.parent));
        f << "      \"fields\": [\n";
        for (size_t i = 0; i < cls.fields.size(); ++i) {
            auto& fld = cls.fields[i];
            f << std::format(
                "        {{ \"name\": \"{}\", \"type\": \"{}\", \"offset\": {}, \"offset_hex\": \"{:#x}\", \"static\": {} }}",
                esc(fld.name), esc(fld.type), fld.offset, fld.offset, fld.is_static ? "true" : "false");
            if (i + 1 < cls.fields.size()) f << ",";
            f << "\n";
        }
        f << "      ],\n";
        f << "      \"methods\": [\n";
        for (size_t i = 0; i < cls.methods.size(); ++i) {
            auto& me = cls.methods[i];
            f << std::format(
                "        {{ \"name\": \"{}\", \"return_type\": \"{}\", \"params\": \"{}\","
                " \"rva\": {}, \"rva_hex\": \"{:#x}\", \"offset\": {}, \"offset_hex\": \"{:#x}\" }}",
                esc(me.name), esc(me.return_type), esc(me.params),
                me.rva, me.rva, me.offset, me.offset);
            if (i + 1 < cls.methods.size()) f << ",";
            f << "\n";
        }
        f << "      ]\n";
        f << "    }";
    }
    f << "\n  ]\n}\n";
    std::cout << std::format("[+] {}\n", out.string());
}

void write_txt(const std::vector<ClassEntry>& classes, const fs::path& out, const std::string& ts) {
    std::ofstream f(out);
    f << std::format("// Generated using https://github.com/Romuilesik/Redmatch-2-Offsets-Dumper-Offsets\n// {}\n\n", ts);

    for (auto& cls : classes) {
        if (cls.fields.empty() && cls.methods.empty()) continue;
        std::string header = cls.full_name();
        if (!cls.parent.empty()) header += " : " + cls.parent;
        f << std::format("Class: {}\n", header);
        for (auto& fld : cls.fields) {
            std::string tag = fld.is_static ? "[static] " : "";
            f << std::format("  [{:#06x}] {}{} : {}\n", fld.offset, tag, fld.name, fld.type);
        }
        for (auto& me : cls.methods) {
            std::string ret = me.return_type.empty() ? "" : std::format(" -> {}", me.return_type);
            f << std::format("  [RVA {:#010x}] {}({}){}\n", me.rva, me.name, me.params, ret);
        }
        f << "\n";
    }
    std::cout << std::format("[+] {}\n", out.string());
}

void write_cs(const std::vector<ClassEntry>& classes, const fs::path& out, const std::string& ts) {
    std::ofstream f(out);
    f << std::format("// Generated using https://github.com/Romuilesik/Redmatch-2-Offsets-Dumper-Offsets\n// {}\n\nnamespace RedMatch2Offsets\n{{\n", ts);

    for (auto& cls : classes) {
        if (cls.fields.empty() && cls.methods.empty()) continue;
        std::string ident = safe_ident(cls.full_name());
        f << std::format("    public static class {}\n    {{\n", ident);
        if (!cls.parent.empty())
            f << std::format("        // parent: {}\n", cls.parent);
        for (auto& fld : cls.fields) {
            std::string fname = safe_ident(fld.name) + (fld.is_static ? "_Static" : "");
            f << std::format("        public const int {} = {:#x}; // {}\n", fname, fld.offset, fld.type);
        }
        for (auto& me : cls.methods) {
            std::string ret = me.return_type.empty() ? "" : std::format(" -> {}", me.return_type);
            f << std::format("        // [RVA {:#x}] {}({}){}\n", me.rva, me.name, me.params, ret);
        }
        f << "    }\n\n";
    }
    f << "}\n";
    std::cout << std::format("[+] {}\n", out.string());
}

void write_rs(const std::vector<ClassEntry>& classes, const fs::path& out, const std::string& ts) {
    std::ofstream f(out);
    f << std::format("// Generated using https://github.com/Romuilesik/Redmatch-2-Offsets-Dumper-Offsets\n// {}\n\n", ts);

    for (auto& cls : classes) {
        if (cls.fields.empty() && cls.methods.empty()) continue;
        std::string mod = safe_ident(cls.full_name());
        std::ranges::transform(mod, mod.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        f << "#[allow(dead_code, non_upper_case_globals)]\n";
        f << std::format("pub mod {} {{\n", mod);
        if (!cls.parent.empty())
            f << std::format("    // parent: {}\n", cls.parent);
        for (auto& fld : cls.fields) {
            std::string fname = safe_ident(fld.name, true) + (fld.is_static ? "_STATIC" : "");
            f << std::format("    pub const {}: usize = {:#x}; // {}\n", fname, fld.offset, fld.type);
        }
        for (auto& me : cls.methods) {
            std::string ret = me.return_type.empty() ? "" : std::format(" -> {}", me.return_type);
            f << std::format("    // [RVA {:#x}] {}({}){}\n", me.rva, me.name, me.params, ret);
        }
        f << "}\n\n";
    }
    std::cout << std::format("[+] {}\n", out.string());
}

int main(int argc, char* argv[]) {
    fs::path input_dir = ".";
    fs::path output_dir = "./offsets";

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if ((arg == "--input" || arg == "-i") && i + 1 < argc)
            input_dir = argv[++i];
        else if ((arg == "--output" || arg == "-o") && i + 1 < argc)
            output_dir = argv[++i];
        else if (arg == "--help" || arg == "-h") {
            std::cout <<
                "Usage: parse [--input <dir>] [--output <dir>]\n"
                "  --input  / -i   folder with dump.cs (default: .)\n"
                "  --output / -o   output folder       (default: ./offsets)\n";
            return 0;
        }
    }

    fs::path dump_cs = input_dir / "dump.cs";
    if (!fs::exists(dump_cs)) {
        std::cerr << std::format("[-] dump.cs not found in: {}\n", input_dir.string());
        std::cerr << "    Run Il2CppDumper first: https://github.com/Perfare/Il2CppDumper\n";
        return 1;
    }

    std::cout << std::format("[*] Parsing {} ...\n", dump_cs.string());
    auto classes = parse_dump_cs(dump_cs);

    if (classes.empty()) {
        std::cerr << "[-] No classes parsed. Check that dump.cs is valid Il2CppDumper output.\n";
        return 1;
    }

    size_t total_fields = 0;
    size_t total_methods = 0;
    for (auto& c : classes) {
        total_fields += c.fields.size();
        total_methods += c.methods.size();
    }
    std::cout << std::format("[*] Found {} classes, {} fields, {} methods\n",
        classes.size(), total_fields, total_methods);

    fs::create_directories(output_dir);
    std::string ts = utc_timestamp();

    write_hpp(classes, output_dir / "rm2_offsets.hpp", ts);
    write_json(classes, output_dir / "rm2_offsets.json", ts);
    write_txt(classes, output_dir / "rm2_dump.txt", ts);
    write_cs(classes, output_dir / "Rm2Offsets.cs", ts);
    write_rs(classes, output_dir / "rm2_offsets.rs", ts);

    std::cout << std::format("\n[+] Done. Output: {}\n", fs::absolute(output_dir).string());
    return 0;
}