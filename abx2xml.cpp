/*
Copyright 2021-2024, CCL Forensics
Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/



/*
While this C++ implementation originates from:

"https://github.com/rhythmcache/android-xml-converter"

most of the inspiration and core logic are adapted from:

"https://github.com/cclgroupltd/android-bits/blob/main/ccl_abx/ccl_abx.py"

Due to this, I am including the original license text above to comply with the
original licensing terms.
*/



#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <memory>
#include <stdexcept>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <algorithm>
#include <cstring>

std::string base64_encode(const unsigned char* data, size_t len) {
    static const char base64_chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve(((len + 2) / 3) * 4);

    for (size_t i = 0; i < len; i += 3) {
        uint32_t triple = (i + 0 < len ? (data[i] << 16) : 0) |
                          (i + 1 < len ? (data[i + 1] << 8) : 0) |
                          (i + 2 < len ? data[i + 2] : 0);

        encoded += base64_chars[(triple >> 18) & 0x3F];
        encoded += base64_chars[(triple >> 12) & 0x3F];
        encoded += (i + 1 < len) ? base64_chars[(triple >> 6) & 0x3F] : '=';
        encoded += (i + 2 < len) ? base64_chars[triple & 0x3F] : '=';
    }
    return encoded;
}

class XMLElement {
public:
    std::string tag;
    std::string text;
    std::unordered_map<std::string, std::string> attrib;
    std::vector<std::shared_ptr<XMLElement>> children;

    XMLElement() = default;
    explicit XMLElement(const std::string& tag_name) : tag(tag_name) {}

    void add_child(const std::shared_ptr<XMLElement>& child) {
        children.push_back(child);
    }
};

enum class XmlType : uint8_t {
    START_DOCUMENT = 0,
    END_DOCUMENT = 1,
    START_TAG = 2,
    END_TAG = 3,
    TEXT = 4,
    ATTRIBUTE = 15
};

enum class DataType : uint8_t {
    TYPE_NULL = 1 << 4,
    TYPE_STRING = 2 << 4,
    TYPE_STRING_INTERNED = 3 << 4,
    TYPE_BYTES_HEX = 4 << 4,
    TYPE_BYTES_BASE64 = 5 << 4,
    TYPE_INT = 6 << 4,
    TYPE_INT_HEX = 7 << 4,
    TYPE_LONG = 8 << 4,
    TYPE_LONG_HEX = 9 << 4,
    TYPE_FLOAT = 10 << 4,
    TYPE_DOUBLE = 11 << 4,
    TYPE_BOOLEAN_TRUE = 12 << 4,
    TYPE_BOOLEAN_FALSE = 13 << 4
};

class AbxDecodeError : public std::runtime_error {
public:
    explicit AbxDecodeError(const std::string& msg) : std::runtime_error(msg) {}
};


class AbxReader {
private:
    std::ifstream stream;
    std::vector<std::string> interned_strings;
    static constexpr char MAGIC[] = "ABX\0";

    uint8_t read_byte() {
        uint8_t byte;
        if (!stream.read(reinterpret_cast<char*>(&byte), 1))
            throw std::runtime_error("Could not read byte");
        return byte;
    }

    int16_t read_short() {
        int16_t val;
        if (!stream.read(reinterpret_cast<char*>(&val), 2))
            throw std::runtime_error("Could not read short");
        return __builtin_bswap16(val);
    }

    uint16_t read_unsigned_short() {
        uint16_t val;
        if (!stream.read(reinterpret_cast<char*>(&val), 2))
            throw std::runtime_error("Could not read unsigned short");
        return __builtin_bswap16(val);
    }

    int32_t read_int() {
        int32_t val;
        if (!stream.read(reinterpret_cast<char*>(&val), 4))
            throw std::runtime_error("Could not read int");
        return __builtin_bswap32(val);
    }

    int64_t read_long() {
        int64_t val;
        if (!stream.read(reinterpret_cast<char*>(&val), 8))
            throw std::runtime_error("Could not read long");
        return __builtin_bswap64(val);
    }

    float read_float() {
        float val;
        if (!stream.read(reinterpret_cast<char*>(&val), 4))
            throw std::runtime_error("Could not read float");
        val = __builtin_bswap32(*reinterpret_cast<int32_t*>(&val));
        return val;
    }

    double read_double() {
        double val;
        if (!stream.read(reinterpret_cast<char*>(&val), 8))
            throw std::runtime_error("Could not read double");
        val = __builtin_bswap64(*reinterpret_cast<int64_t*>(&val));
        return val;
    }

    std::string read_string_raw() {
        uint16_t length = read_unsigned_short();
        std::vector<char> buffer(length);
        if (!stream.read(buffer.data(), length))
            throw std::runtime_error("Could not read string");
        return std::string(buffer.begin(), buffer.end());
    }

    std::string read_interned_string() {
        int16_t reference = read_short();
        if (reference == -1) {
            std::string value = read_string_raw();
            interned_strings.push_back(value);
            return value;
        }
        return interned_strings[reference];
    }

    void skip_header_extension() {
        // Read and skip any extension data after the magic number
        while (true) {
            uint8_t token = read_byte();
            if ((token & 0x0f) == static_cast<uint8_t>(XmlType::START_DOCUMENT)) {
                // Found the start of the actual document
                stream.seekg(-1, std::ios::cur);  // Go back one byte
                break;
            }
            
            // Skip extension data based on type
            uint8_t data_type = token & 0xf0;
            switch (static_cast<DataType>(data_type)) {
                case DataType::TYPE_NULL:
                    break;
                case DataType::TYPE_INT:
                    read_int();
                    break;
                case DataType::TYPE_LONG:
                    read_long();
                    break;
                case DataType::TYPE_FLOAT:
                    read_float();
                    break;
                case DataType::TYPE_DOUBLE:
                    read_double();
                    break;
                case DataType::TYPE_STRING:
                case DataType::TYPE_STRING_INTERNED:
                    read_string_raw();
                    break;
                case DataType::TYPE_BYTES_HEX:
                case DataType::TYPE_BYTES_BASE64:
                    {
                        uint16_t length = read_short();
                        stream.seekg(length, std::ios::cur);
                    }
                    break;
                default:
                    // For unknown types, try to skip based on the lower 4 bits
                    if ((token & 0x0f) > 0) {
                        stream.seekg(token & 0x0f, std::ios::cur);
                    }
                    break;
            }
        }
    }

public:
    explicit AbxReader(const std::string& filename) {
        stream.open(filename, std::ios::binary);
        if (!stream)
            throw std::runtime_error("Could not open file");
    }

    std::shared_ptr<XMLElement> read(bool is_multi_root = false) {
        // Validate magic number
        char magic_check[4];
        if (!stream.read(magic_check, 4) || memcmp(magic_check, MAGIC, 4) != 0)
            throw AbxDecodeError("Invalid magic number");

        // Skip any header extension data
        skip_header_extension();

        bool document_opened = true;
        bool root_closed = false;
        std::vector<std::shared_ptr<XMLElement>> element_stack;
        std::shared_ptr<XMLElement> root;

        if (is_multi_root) {
            root = std::make_shared<XMLElement>("root");
            element_stack.push_back(root);
        }

        while (true) {
            if (stream.eof())
                break;

            uint8_t token = read_byte();
            uint8_t xml_type = token & 0x0f;
            uint8_t data_type = token & 0xf0;

            if (xml_type == static_cast<uint8_t>(XmlType::START_DOCUMENT)) {
                if (data_type != static_cast<uint8_t>(DataType::TYPE_NULL))
                    throw AbxDecodeError("Invalid START_DOCUMENT data type");
                document_opened = true;
            }
            else if (xml_type == static_cast<uint8_t>(XmlType::END_DOCUMENT)) {
                if (data_type != static_cast<uint8_t>(DataType::TYPE_NULL))
                    throw AbxDecodeError("Invalid END_DOCUMENT data type");
                if (!(element_stack.empty() || (is_multi_root && element_stack.size() == 1)))
                    throw AbxDecodeError("Unclosed elements at END_DOCUMENT");
                break;
            }
            else if (xml_type == static_cast<uint8_t>(XmlType::START_TAG)) {
                if (data_type != static_cast<uint8_t>(DataType::TYPE_STRING_INTERNED))
                    throw AbxDecodeError("Invalid START_TAG data type");

                std::string tag_name = read_interned_string();
                auto element = std::make_shared<XMLElement>(tag_name);

                if (element_stack.empty()) {
                    root = element;
                    element_stack.push_back(element);
                } else {
                    element_stack.back()->add_child(element);
                    element_stack.push_back(element);
                }
            }
            else if (xml_type == static_cast<uint8_t>(XmlType::END_TAG)) {
                if (data_type != static_cast<uint8_t>(DataType::TYPE_STRING_INTERNED))
                    throw AbxDecodeError("Invalid END_TAG data type");

                if (element_stack.empty() || (is_multi_root && element_stack.size() == 1))
                    throw AbxDecodeError("Unexpected END_TAG");

                std::string tag_name = read_interned_string();
                if (element_stack.back()->tag != tag_name)
                    throw AbxDecodeError("Mismatched END_TAG");

                element_stack.pop_back();
                if (element_stack.empty())
                    root_closed = true;
            }
            else if (xml_type == static_cast<uint8_t>(XmlType::TEXT)) {
                std::string value = read_string_raw();

                // Ignore whitespace
                if (std::all_of(value.begin(), value.end(), ::isspace))
                    continue;

                if (element_stack.empty())
                    throw AbxDecodeError("Unexpected TEXT outside of element");

                if (element_stack.back()->text.empty())
                    element_stack.back()->text = value;
                else
                    element_stack.back()->text += value;
            }
            else if (xml_type == static_cast<uint8_t>(XmlType::ATTRIBUTE)) {
                if (element_stack.empty() || (is_multi_root && element_stack.size() == 1))
                    throw AbxDecodeError("Unexpected ATTRIBUTE");

                std::string attribute_name = read_interned_string();
                std::string value;

                switch (static_cast<DataType>(data_type)) {
                    case DataType::TYPE_NULL:
                        value = "null";
                        break;
                    case DataType::TYPE_BOOLEAN_TRUE:
                        value = "true";
                        break;
                    case DataType::TYPE_BOOLEAN_FALSE:
                        value = "false";
                        break;
                    case DataType::TYPE_INT:
                        value = std::to_string(read_int());
                        break;
                    case DataType::TYPE_INT_HEX: {
                        std::stringstream ss;
                        ss << std::hex << read_int();
                        value = ss.str();
                        break;
                    }
                    case DataType::TYPE_LONG:
                        value = std::to_string(read_long());
                        break;
                    case DataType::TYPE_LONG_HEX: {
                        std::stringstream ss;
                        ss << std::hex << read_long();
                        value = ss.str();
                        break;
                    }
                    case DataType::TYPE_FLOAT:
                        value = std::to_string(read_float());
                        break;
                    case DataType::TYPE_DOUBLE:
                        value = std::to_string(read_double());
                        break;
                    case DataType::TYPE_STRING:
                        value = read_string_raw();
                        break;
                    case DataType::TYPE_STRING_INTERNED:
                        value = read_interned_string();
                        break;
                    case DataType::TYPE_BYTES_HEX: {
                        uint16_t length = read_short();
                        std::vector<unsigned char> buffer(length);
                        if (!stream.read(reinterpret_cast<char*>(buffer.data()), length))
                            throw std::runtime_error("Could not read bytes");
                        
                        std::stringstream ss;
                        for (auto byte : buffer)
                            ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
                        value = ss.str();
                        break;
                    }
                    case DataType::TYPE_BYTES_BASE64: {
                        uint16_t length = read_short();
                        std::vector<unsigned char> buffer(length);
                        if (!stream.read(reinterpret_cast<char*>(buffer.data()), length))
                            throw std::runtime_error("Could not read bytes");
                        
                        value = base64_encode(buffer.data(), buffer.size());
                        break;
                    }
                    default:
                        throw AbxDecodeError("Unexpected attribute data type");
                }

                element_stack.back()->attrib[attribute_name] = value;
            }
            else {
                // Try to skip unknown token types
                if (data_type != 0) {
                    switch (static_cast<DataType>(data_type)) {
                        case DataType::TYPE_INT:
                            read_int();
                            break;
                        case DataType::TYPE_STRING:
                        case DataType::TYPE_STRING_INTERNED:
                            read_string_raw();
                            break;
                        default:
                            throw AbxDecodeError("Unexpected XML type");
                    }
                }
            }
        }

        if (!root)
            throw AbxDecodeError("No root element found");

        return root;
    }

    void print_xml(const std::shared_ptr<XMLElement>& element, int indent = 0) {
    if (indent == 0) {  // Only print declaration for root element
        std::cout << "<?xml version='1.0' encoding='UTF-8' standalone='yes' ?>\n";
    }
        
        std::string indentation(indent, ' ');
        std::cout << indentation << "<" << element->tag;
        
        for (const auto& [key, value] : element->attrib)
            std::cout << " " << key << "=\"" << value << "\"";
        
        if (element->children.empty() && element->text.empty()) {
            std::cout << "/>" << std::endl;
            return;
        }
        
        std::cout << ">";
        
        if (!element->text.empty())
            std::cout << element->text;
        
        if (!element->children.empty()) {
            std::cout << std::endl;
            for (const auto& child : element->children)
                print_xml(child, indent + 2);
            std::cout << indentation;
        }
        
        std::cout << "</" << element->tag << ">" << std::endl;
    }
};



void print_usage() {
    std::cerr << "usage: abx2xml [-mr] [-i] input [output]\n\n"
              << "Converts between human-readable XML and Android Binary XML.\n\n"
              << " [-mr] : Enable Multi-Root Processing.\n\n"
              << "When invoked with the '-i' argument, the output of a successful conversion\n"
              << "will overwrite the original input file. output can be '-' to use stdout\n\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    bool multi_root = false;
    std::string input_path;
    std::string output_path;
    bool explicit_input = false;
    bool output_to_stdout = false;
    
    // Argument parsing
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "-mr") {
            multi_root = true;
        } 
        else if (arg == "-i") {
            explicit_input = true;
        } 
        else if (input_path.empty()) {
            input_path = arg;
        } 
        else if (output_path.empty()) {
            output_path = arg;
        } 
        else {
            std::cerr << "Error: Too many arguments\n";
            print_usage();
            return 1;
        }
    }

    // Validation and path handling
    if (input_path.empty()) {
        std::cerr << "Error: No input file specified\n";
        print_usage();
        return 1;
    }
    
    if (output_path.empty()) {
        if (explicit_input) {
            // When -i is used, overwrite input file
            output_path = input_path;
        } else {
            output_path = input_path;
            size_t dot_pos = output_path.find_last_of('.');
            if (dot_pos != std::string::npos) {
                output_path = output_path.substr(0, dot_pos) + ".xml";
            } else {
                output_path += ".xml";
            }
        }
    }
    
    output_to_stdout = (output_path == "-");
    if (output_to_stdout) {
        output_path = input_path;
    }

    try {
        AbxReader reader(input_path);
        auto doc = reader.read(multi_root);

        if (output_to_stdout) {
            reader.print_xml(doc);
        } else {
            std::ofstream output_file(output_path, std::ios::out | std::ios::trunc);
            if (!output_file) {
                std::cerr << "Error: Could not open output file '" << output_path << "'\n";
                return 1;
            }
            
            auto old_buf = std::cout.rdbuf(output_file.rdbuf());
            reader.print_xml(doc);
            std::cout.rdbuf(old_buf);
        }

        std::cerr << "Successfully converted " << input_path 
                   << " to " << output_path 
                   << (multi_root ? " (multi-root mode)" : "") 
                   << std::endl;
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
