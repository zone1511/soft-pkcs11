
#include <stdio.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>

#include <iostream>
#include <fstream>
#include <functional>

#include <boost/bind.hpp>
#include <boost/iterator/filter_iterator.hpp>
#include <boost/iterator/transform_iterator.hpp>

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/filesystem.hpp>

#include "tools.h"
#include "soft_token.h"

enum Attribute : CK_ATTRIBUTE_TYPE {
    AttrFilename = CKA_VENDOR_DEFINED + 1,
    AttrFullpath,
    AttrSshPublic,
};

const CK_BBOOL bool_true = CK_TRUE;
const CK_BBOOL bool_false = CK_FALSE;

namespace fs = boost::filesystem;

struct descriptor_t;
typedef std::shared_ptr<descriptor_t> descriptor_p;

Attributes data_object_attrs(descriptor_p desc, const Attributes& attributes = Attributes());
Attributes public_key_attrs(descriptor_p desc,  const Attributes& attributes = Attributes());    
Attributes rsa_public_key_attrs(descriptor_p desc,  const Attributes& attributes = Attributes());
Attributes ssh_public_key_attrs(descriptor_p desc,  const Attributes& attributes = Attributes());    
Attributes private_key_attrs(descriptor_p desc, const Attributes& attributes = Attributes());
Attributes rsa_private_key_attrs(descriptor_p desc, const Attributes& attributes = Attributes());
Attributes secret_key_attrs(descriptor_p desc,  const Attributes& attributes = Attributes());

struct is_object : std::unary_function<const fs::directory_entry&, bool> {
    bool operator() (const fs::directory_entry& d) const {
        return fs::is_regular_file(d.status());
    }
};

struct to_object_id : std::unary_function<const fs::directory_entry&, CK_OBJECT_HANDLE> {
    CK_OBJECT_HANDLE operator() (const fs::directory_entry& d) const {
        return static_cast<CK_OBJECT_HANDLE>(hash(d.path().filename().c_str()));
    }
private:
    std::hash<std::string> hash;
};

struct descriptor_t {
    descriptor_t(const fs::directory_entry& d)
        : fullname(d.path().string())
        , filename(d.path().filename().string())
    {
        std::ifstream stream1(d.path().string());
        std::getline(stream1, first_line, '\n');
        stream1.seekg (0, stream1.beg);
        
        std::ifstream stream(d.path().string());
        
        data = std::vector<char>((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        id = to_object_id()(d);
        file = ::fmemopen(data.data(), data.size(), "r");
    }
    
    ~descriptor_t() {
        ::fclose(file);
    }
    
    const std::string fullname;
    const std::string filename;
    std::vector<char> data;
    std::string first_line;
    CK_OBJECT_HANDLE id;
    FILE *file;
};

struct is_public_key : std::unary_function<descriptor_p, bool> {
    bool operator() (descriptor_p desc) {
      return desc->first_line.find("ssh-rsa") == 0
        || desc->first_line == "-----BEGIN PUBLIC KEY-----"
        || desc->first_line == "-----BEGIN RSA PUBLIC KEY-----";        
    }
};

struct is_rsa_public_key : std::unary_function<descriptor_p, bool> {
    bool operator() (descriptor_p desc) {
      return desc->first_line == "-----BEGIN PUBLIC KEY-----"
        || desc->first_line == "-----BEGIN RSA PUBLIC KEY-----";        
    }
};

struct is_ssh_public_key : std::unary_function<descriptor_p, bool> {
    bool operator() (descriptor_p desc) {
      return desc->first_line.find("ssh-rsa") == 0;
    }
};

struct is_private_key : std::unary_function<descriptor_p, bool> {
    bool operator() (descriptor_p desc) {
        return desc->first_line == "-----BEGIN RSA PRIVATE KEY-----";        
    }
};

struct is_rsa_private_key : std::unary_function<descriptor_p, bool> {
    bool operator() (descriptor_p desc) {
        return desc->first_line == "-----BEGIN RSA PRIVATE KEY-----";        
    }
};

struct to_attributes : std::unary_function<const fs::directory_entry&, Objects::value_type> {
    
    Objects& objects;
    to_attributes(Objects& o): objects(o) {
        
    }
    
    Objects::value_type operator() (const fs::directory_entry& d) {
        
        descriptor_p desc(new descriptor_t(d));
        
        std::cout << desc->fullname << std::endl;
        
        Attributes attrs = {
            create_object(AttrFilename, desc->filename),
            create_object(AttrFullpath, desc->fullname),
        };
        
        attrs = data_object_attrs(desc, attrs);
        
        if (is_public_key()(desc)) {
            attrs = public_key_attrs(desc, attrs);
        }
        if (is_rsa_public_key()(desc)) {
            attrs = rsa_public_key_attrs(desc, attrs);
        }
        if (is_ssh_public_key()(desc)) {
            attrs = ssh_public_key_attrs(desc, attrs);
            attrs[AttrSshPublic] = attribute_t(AttrSshPublic, bool_false);
            attrs[CKA_OBJECT_ID] = attribute_t(CKA_OBJECT_ID, desc->id + 10);
            attrs[CKA_ID] = attribute_t(CKA_ID, desc->id + 10);
            
            objects.insert(std::make_pair(desc->id + 10, attrs));
            
            attrs[CKA_OBJECT_ID] = attribute_t(CKA_OBJECT_ID, desc->id);
            attrs[CKA_ID] = attribute_t(CKA_ID, desc->id);
            attrs[CKA_LABEL] = attribute_t(CKA_LABEL, "SSH " + attrs[CKA_LABEL].to_string());
            attrs[AttrSshPublic] = attribute_t(AttrSshPublic, bool_true);
        }
        
        if (is_private_key()(desc)) {
            attrs = private_key_attrs(desc, attrs);
        }
        if (is_rsa_private_key()(desc)) {
            attrs = rsa_private_key_attrs(desc, attrs);
        }
        
        return std::make_pair(desc->id, attrs);
    }
};

struct find_by_attrs : std::unary_function<const Objects::value_type, bool> {
    find_by_attrs(const Attributes& a) : attrs(a) {}
    
    bool operator()(const Objects::value_type object_pair) const {
        
        st_logf("SEARCH FOR ID: %lu\n", object_pair.first);        
        
        for (auto it = attrs.begin(); it != attrs.end(); ++it) {
            const Attributes& object_attrs = object_pair.second;
            
            st_logf("----- compare attr type:: %d\n", it->first);        
            
            auto fnd = object_attrs.find(it->first);
            if (fnd != object_attrs.end()) {
                if (fnd->second != it->second) {
                    st_logf("attr type %d NOT EQUAL\n", it->first);
                    return false;
                }
            }
            else {
                st_logf("attr type %d NOT FOUND\n", it->first);
                return false;
            }
        }
        
        st_logf("object MATCH\n");
        return true;
    };
    
private:
    const Attributes attrs;
};

typedef boost::filter_iterator<std::function<bool(const fs::directory_entry&)>, fs::directory_iterator> files_iterator;
typedef boost::transform_iterator<to_object_id, files_iterator> object_ids_iterator;

typedef std::function<bool(const Objects::value_type&)> ObjectsPred;

struct soft_token_t::Pimpl {
  
    Pimpl() {
      config.put("path", "default");
    }
    
    /// Iterate over all files in path
    files_iterator files_begin() const {
        if (fs::exists(path) && fs::is_directory(path)) {
            return files_iterator(is_object(), fs::directory_iterator(path));
        }    
        
        return files_end();
    };
    
    /// end-iterator
    files_iterator files_end() const {
        return files_iterator(fs::directory_iterator());
    }
    
    
    /// Find in objects by predicate
    Objects::const_iterator find(std::function<bool(const Attributes&)> pred) const {
        return std::find_if(objects.begin(), objects.end(), [&pred] (const Objects::value_type& v) {
            return pred(v.second);
        });
    }
    
    /// Find in objects by predicate
    Objects::iterator find(std::function<bool(const Attributes&)> pred) {
        return std::find_if(objects.begin(), objects.end(), [&pred] (const Objects::value_type& v) {
            return pred(v.second);
        });
    }
    
    
//     
//     boost::filter_iterator<ObjectsPred, Objects::const_iterator> filter_iterator(ObjectsPred pred) const {
//         return boost::filter_iterator<ObjectsPred, Objects::const_iterator>(pred, objects.begin(), objects.end());
//     }
//     
//     /// Filter objects by predicate
//     boost::filter_iterator<ObjectsPred, Objects::iterator> filter_iterator(ObjectsPred pred) {
//         return boost::filter_iterator<ObjectsPred, Objects::iterator>(pred, objects.begin(), objects.end());
//     }

    /// Filter objects by predicate
    template <typename It = Objects::iterator>
    boost::filter_iterator<ObjectsPred, It> filter_iterator(ObjectsPred pred, It b = It(), It e = It()) {
        if (b == It()) {b = objects.begin();}
        if (e == It()) {e = objects.end();}
        
        return boost::filter_iterator<ObjectsPred, It>(pred, b, e);
    }
    
//     /// Filter objects by predicate
//     template <typename It = Objects::const_iterator>
//     boost::filter_iterator<ObjectsPred, It> filter_iterator(ObjectsPred pred, It b = It(), It e = It()) const {
//         if (b == It()) {b = objects.begin();}
//         if (e == It()) {e = objects.end();}
//         
//         return boost::filter_iterator<ObjectsPred, It>(pred, b, e);
//     }
    
    /// Filter objects by attributes
    template <typename It = Objects::iterator>
    boost::filter_iterator<ObjectsPred, It> filter_iterator(const Attributes& attrs, It b = It(), It e = It()) {
        if (b == It()) {b = objects.begin();}
        if (e == It()) {e = objects.end();}
        
        return boost::filter_iterator<ObjectsPred, It>(find_by_attrs(attrs), b, e);
    }
    
//     /// Filter objects by attributes
//     template <typename It = Objects::const_iterator>
//     boost::filter_iterator<ObjectsPred, It> filter_iterator(const Attributes& attrs, It b = It(), It e = It()) const {
//         if (b == It()) {b = objects.begin();}
//         if (e == It()) {e = objects.end();}
//         
//         return boost::filter_iterator<ObjectsPred, It>(find_by_attrs(attrs), b, e);
//     }
//     
    
    

    /// Filter end iterator
    template <typename It = Objects::iterator>
    boost::filter_iterator<ObjectsPred, It> filter_end(It e = It()) {
        if (e == It()) {e = objects.end();}
        
        return boost::filter_iterator<ObjectsPred, It>(ObjectsPred(), e, e);
    }
    
//     /// Filter end iterator
//     boost::filter_iterator<ObjectsPred, Objects::iterator> filter_end() {
//         return boost::filter_iterator<ObjectsPred, Objects::iterator>(ObjectsPred(), objects.end(), objects.end());
//     }
//     


    
    /// Iterate over transformed(through trans-function) collection
    template<typename Trans, typename It = Objects::const_iterator>
    boost::transform_iterator<Trans, It> trans_iterator(Trans trans, It b) const {
        return boost::transform_iterator<Trans, It>(b, trans);
    }
    
    /// Transformed end-iterator
    template<typename Trans, typename It = Objects::const_iterator>
    boost::transform_iterator<Trans, It> trans_end(Trans trans, It e) const {
        return boost::transform_iterator<Trans, It>(e, trans);
    }

    std::vector<int> vi;
  
    boost::property_tree::ptree config;
    std::string path;
    Objects objects;
    std::string pin;
};

int read_password (char *buf, int size, int rwflag, void *userdata) {
    std::string p;
    std::cin >> p;
    std::copy_n(p.begin(), std::min(size, static_cast<int>(p.size())), buf);
    return p.size();
}


/*
bool check_file_is_private_key(const std::string& file) {
    std::ifstream infile(file);
    std::string first_line;
    std::getline(infile, first_line, '\n');
    return first_line == "-----BEGIN RSA PRIVATE KEY-----";
}*/

soft_token_t::soft_token_t(const std::string& rcfile)
    : p_(new Pimpl())
{
   
    try {
      boost::property_tree::ini_parser::read_ini(rcfile, p_->config);
    }
    catch (...) {}
    
    p_->path = p_->config.get<std::string>("path");

    st_logf("Config file: %s\n", rcfile.c_str());
    st_logf("Path : %s\n", p_->path.c_str());
    
    const auto end = p_->files_end();
    to_attributes convert(p_->objects);
    for(auto it = p_->files_begin(); it != end; ++it ) {
        const auto a = p_->objects.insert(convert(*it)).first;
        st_logf("Finded obejcts: %s %lu\n", it->path().filename().c_str(), a->first);
    }
    
    const CK_OBJECT_CLASS public_key = CKO_PUBLIC_KEY;
    const CK_OBJECT_CLASS private_key = CKO_PRIVATE_KEY;
    
    for(auto private_it = p_->filter_iterator({create_object(CKA_CLASS, private_key)}); private_it != p_->filter_end(); ++private_it) {
//         auto public_it = std::find_if(
//             p_->filter_iterator({create_object(CKA_CLASS, public_key)}),
//             p_->filter_end(),
//             [&private_it](Objects::value_type& pub_key){
//                 return (pub_key.second[CKA_LABEL].to_string() == (private_it->second[CKA_LABEL].to_string() + ".pub")) || 
//                 pub_key.second[CKA_MODULUS] == private_it->second[CKA_MODULUS];
//             }
//         );
        
        auto public_it = p_->filter_iterator([&private_it] (const Objects::value_type& pub_key) mutable {
            return (pub_key.second[CKA_LABEL].to_string() == (private_it->second[CKA_LABEL].to_string() + ".pub")) || 
                pub_key.second[CKA_MODULUS] == private_it->second[CKA_MODULUS];
        },
            p_->filter_iterator({create_object(CKA_CLASS, public_key)}),
            p_->filter_end()
        );
        
        auto public_end = p_->filter_end(p_->filter_end());
        
//         for()
        
//         if (public_it != p_->filter_end()) {
//             public_it->second[CKA_ID] = private_it->second[CKA_ID];
//         }
    }

    st_logf("Invalid obejct: %lu\n", this->handle_invalid());
    
}

soft_token_t::~soft_token_t()
{
//     std::cerr << "DESTRUCTOR 1" << std::endl;
    p_.reset();
//     std::cerr << "DESTRUCTOR 2" << std::endl;
}

bool soft_token_t::logged() const
{
    return !p_->pin.empty();
}

bool soft_token_t::login(const std::string& pin)
{
    p_->pin = pin;
    return logged();
}

Handles soft_token_t::handles() const
{
    return Handles(
        p_->trans_iterator(boost::bind(&Objects::value_type::first,_1), p_->objects.begin()),
        p_->trans_end(boost::bind(&Objects::value_type::first,_1), p_->objects.end())
    );
}

handle_iterator_t soft_token_t::handles_iterator() const
{
    auto it = p_->trans_iterator(boost::bind(&Objects::value_type::first,_1), p_->objects.begin());
    auto end = p_->trans_end(boost::bind(&Objects::value_type::first,_1), p_->objects.end());
    
    return handle_iterator_t([it, end] () mutable {
        if (it != end) {
            return *(it++);
        }
        else {
            soft_token_t::handle_invalid();
        }
    });
}



handle_iterator_t soft_token_t::find_handles_iterator(Attributes attrs) const
{
    auto it = p_->trans_iterator(boost::bind(&Objects::value_type::first,_1), p_->filter_iterator(attrs));
    auto end = p_->trans_end(boost::bind(&Objects::value_type::first,_1), p_->filter_end());
    
    return handle_iterator_t([it, end] () mutable {
        if (it != end) {
            return *(it++);
        }
        else {
            return soft_token_t::handle_invalid();
        }
    });
}

CK_OBJECT_HANDLE soft_token_t::handle_invalid()
{
    return static_cast<CK_OBJECT_HANDLE>(-1);  
}


Attributes soft_token_t::attributes(CK_OBJECT_HANDLE id) const
{
    auto it = p_->objects.find(id);
    
    if (it != p_->objects.end()) {
        return it->second;
    }
    
    return Attributes();
}

std::string soft_token_t::read(CK_OBJECT_HANDLE id) const
{
    auto it = p_->objects.find(id);
    
    if (it != p_->objects.end()) {
        std::ifstream t(it->second[AttrFullpath].to_string());
        return std::string((std::istreambuf_iterator<char>(t)), std::istreambuf_iterator<char>());
    }

    return std::string();
}




Attributes data_object_attrs(descriptor_p desc, const Attributes& attributes)
{
    const CK_OBJECT_CLASS klass = CKO_DATA;
    const CK_FLAGS flags = 0;
    
    
    Attributes attrs = {
        create_object(CKA_CLASS,     klass),

        //Common Storage Object Attributes
        create_object(CKA_TOKEN,     bool_true),
        create_object(CKA_PRIVATE,   bool_true),
        create_object(CKA_MODIFIABLE,bool_false),
        create_object(CKA_LABEL,     desc->filename),
        
        //Data Object Attributes
        //create_object(CKA_APPLICATION, desc->id),
        create_object(CKA_OBJECT_ID, desc->id),
        //create_object(CKA_VALUE, desc->id), //read when needed
    };

    //keys in attrs takes precedence with attributes
    attrs.insert(attributes.begin(), attributes.end());
    
    return attrs;
}

Attributes public_key_attrs(descriptor_p desc, const Attributes& attributes)
{
    const CK_OBJECT_CLASS klass = CKO_PUBLIC_KEY;
    const CK_MECHANISM_TYPE mech_type = CKM_RSA_X_509;
    
    //ftp://ftp.rsasecurity.com/pub/pkcs/pkcs-11/v2-20/pkcs-11v2-20.pdf
    Attributes attrs = {
        create_object(CKA_CLASS,     klass),
        
        //Common Storage Object Attributes
        create_object(CKA_TOKEN,     bool_true),
        create_object(CKA_PRIVATE,   bool_false),
        create_object(CKA_MODIFIABLE,bool_false),
        create_object(CKA_LABEL,     desc->filename),
        
        //Common Key Attributes
        //create_object(CKA_KEY_TYPE,  type),
        create_object(CKA_ID,        desc->id),
        //create_object(CKA_START_DATE,        id),
        //create_object(CKA_END_DATE,        id),
        create_object(CKA_DERIVE,    bool_false),
        create_object(CKA_LOCAL,     bool_false),
        create_object(CKA_KEY_GEN_MECHANISM, mech_type),
        
        //Common Public Key Attributes
        //create_object(CKA_SUBJECT,   bool_true),
        create_object(CKA_ENCRYPT,   bool_true),
        create_object(CKA_VERIFY,    bool_true),
        //create_object(CKA_VERIFY_RECOVER,   bool_false),
        //create_object(CKA_TRUSTED10,   bool_true),
        //create_object(CKA_WRAP_TEMPLATE ,   bool_true),
        
        /////////////

    };
    
   
    //keys in attrs takes precedence with attributes
    attrs.insert(attributes.begin(), attributes.end());

    return attrs;    
}

Attributes rsa_public_key_attrs(descriptor_p desc, const Attributes& attributes)
{
    const CK_KEY_TYPE type = CKK_RSA;
    
    Attributes attrs = {
        create_object(CKA_KEY_TYPE,  type),
    };
    
    if (EVP_PKEY *pkey = PEM_read_PUBKEY(desc->file, NULL, NULL, NULL)) {
        int size = 0;
        std::shared_ptr<unsigned char> buf;
        
        std::tie(size, buf) = read_bignum(pkey->pkey.rsa->n);
        attrs.insert(std::make_pair(CKA_MODULUS, attribute_t(CKA_MODULUS, buf.get(), size)));
        attrs.insert(create_object(CKA_MODULUS_BITS,   size * 8));            
        
        std::tie(size, buf) = read_bignum(pkey->pkey.rsa->e);
        attrs.insert(std::make_pair(CKA_PUBLIC_EXPONENT, attribute_t(CKA_PUBLIC_EXPONENT, buf.get(), size)));

        EVP_PKEY_free(pkey);
    }
    
    //keys in attrs takes precedence with attributes
    attrs.insert(attributes.begin(), attributes.end());

    return attrs;  
}

Attributes ssh_public_key_attrs(descriptor_p desc, const Attributes& attributes)
{
    FILE* reserve = desc->file;
    
    if (FILE* converted = ::popen(std::string("ssh-keygen -f " + desc->fullname + " -e -m PKCS8").c_str(), "r")) {
        desc->file = converted;
        return rsa_public_key_attrs(desc, attributes);
        ::pclose(converted);
    }
    
    desc->file = reserve;
    
    return attributes;  
}

Attributes private_key_attrs(descriptor_p desc, const Attributes& attributes)
{
    const CK_OBJECT_CLASS klass = CKO_PRIVATE_KEY;
    const CK_MECHANISM_TYPE mech_type = CKM_RSA_X_509;
    const CK_KEY_TYPE type = CKK_GENERIC_SECRET;
    
    Attributes attrs = {
        create_object(CKA_CLASS,     klass),
        
//         std::make_pair(CKA_VALUE, attribute_t(CKA_VALUE, data.size())), // SPECIAL CASE FOR VALUE
        
        //Common Storage Object Attributes
        create_object(CKA_TOKEN,     bool_true),
        create_object(CKA_PRIVATE,   bool_true),
        create_object(CKA_MODIFIABLE,bool_false),
        create_object(CKA_LABEL,     desc->filename),
        
        //Common Key Attributes
        create_object(CKA_KEY_TYPE,  type),
        create_object(CKA_ID,        desc->id),
        //create_object(CKA_START_DATE,      id),
        //create_object(CKA_END_DATE,        id),
        create_object(CKA_DERIVE,    bool_false),
        create_object(CKA_LOCAL,     bool_false),
        create_object(CKA_KEY_GEN_MECHANISM, mech_type),
        
        //Common Private Key Attributes
        //create_object(CKA_SUBJECT,   bool_true),
        create_object(CKA_SENSITIVE, bool_true),
        create_object(CKA_DECRYPT,   bool_true),
        create_object(CKA_SIGN,      bool_true),
        create_object(CKA_SIGN_RECOVER, bool_false),
        create_object(CKA_UNWRAP,    bool_true),
        create_object(CKA_EXTRACTABLE, bool_true),
        //create_object(CKA_ALWAYS_SENSITIVE, bool_true),
        create_object(CKA_NEVER_EXTRACTABLE, bool_false),
        //create_object(CKA_WRAP_WITH_TRUSTED1, bool_false),
        //create_object(CKA_UNWRAP_TEMPLATE, bool_false),
        create_object(CKA_ALWAYS_AUTHENTICATE, bool_true),
        
        /////////////

    };
    
    //keys in attrs takes precedence with attributes 
    attrs.insert(attributes.begin(), attributes.end());

    return attrs;  
}

Attributes rsa_private_key_attrs(descriptor_p desc, const Attributes& attributes) {
    const CK_KEY_TYPE type = CKK_RSA;
    
    Attributes attrs = {
        create_object(CKA_KEY_TYPE,  type),
    };
    
    if (EVP_PKEY *pkey = PEM_read_PrivateKey(desc->file, NULL, NULL, "")) {
        int size = 0;
        std::shared_ptr<unsigned char> buf;
        
        std::tie(size, buf) = read_bignum(pkey->pkey.rsa->n);
        attrs.insert(std::make_pair(CKA_MODULUS, attribute_t(CKA_MODULUS, buf.get(), size)));
        
        std::tie(size, buf) = read_bignum(pkey->pkey.rsa->e);
        attrs.insert(std::make_pair(CKA_PUBLIC_EXPONENT, attribute_t(CKA_PUBLIC_EXPONENT, buf.get(), size)));

        EVP_PKEY_free(pkey);
    }
    
    //keys in attrs takes precedence with attributes
    attrs.insert(attributes.begin(), attributes.end());

    return attrs; 
}

Attributes secret_key_attrs(descriptor_p desc, const Attributes& attributes)
{
    const CK_OBJECT_CLASS klass = CKO_SECRET_KEY;
    const CK_MECHANISM_TYPE mech_type = CKM_RSA_X_509;
    
    //ftp://ftp.rsasecurity.com/pub/pkcs/pkcs-11/v2-20/pkcs-11v2-20.pdf
    Attributes attrs = {
        create_object(CKA_CLASS,     klass),
        
//         std::make_pair(CKA_VALUE, attribute_t(CKA_VALUE, data.size())), // SPECIAL CASE FOR VALUE
        
        //Common Storage Object Attributes
        create_object(CKA_TOKEN,     bool_true),
        create_object(CKA_PRIVATE,   bool_true),
        create_object(CKA_MODIFIABLE,bool_false),
        create_object(CKA_LABEL,     desc->filename),
        
        //Common Key Attributes
        //create_object(CKA_KEY_TYPE,        id),
        create_object(CKA_ID,        desc->id),
        //create_object(CKA_START_DATE,        id),
        //create_object(CKA_END_DATE,        id),
        create_object(CKA_DERIVE,    bool_false),
        create_object(CKA_LOCAL,     bool_false),
        create_object(CKA_KEY_GEN_MECHANISM, mech_type),
        
        //Common Secret Key Attributes
        create_object(CKA_SENSITIVE,      bool_true), //bool_false
        create_object(CKA_ENCRYPT,   bool_true),
        create_object(CKA_DECRYPT,   bool_true),
        create_object(CKA_SIGN,      bool_true),
        create_object(CKA_VERIFY,    bool_false),
        create_object(CKA_WRAP,      bool_false),
        create_object(CKA_UNWRAP,    bool_false),
        create_object(CKA_EXTRACTABLE, bool_true),
        //create_object(CKA_ALWAYS_SENSITIVE, bool_true),
        create_object(CKA_NEVER_EXTRACTABLE, bool_false),
        //create_object(CKA_CHECK_VALUE, bool_false),
        //create_object(CKA_WRAP_WITH_TRUSTED, bool_false),
        //create_object(CKA_TRUSTED, bool_false),
        //create_object(CKA_WRAP_TEMPLATE, bool_false),
        //create_object(CKA_UNWRAP_TEMPLATE, bool_false),
        create_object(CKA_ALWAYS_AUTHENTICATE, bool_true),
        
        /////////////

    };

    //keys in attrs takes precedence with attributes
    attrs.insert(attributes.begin(), attributes.end());

    return attrs;
}









