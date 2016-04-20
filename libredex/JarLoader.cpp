/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */
#if !defined(__MINGW32__)
#include <sys/mman.h>
#endif
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <zlib.h>
#include <vector>
#include "DexClass.h"
#include "JarLoader.h"
#include "Creators.h"

/******************
 * Begin Class Loading code.
 */

namespace {

static const uint32_t kClassMagic = 0xcafebabe;

struct cp_entry {
  uint8_t tag;
  union {
    struct {
      uint16_t s0;
      uint16_t s1;
    };
    struct {
      uint32_t i0;
      uint32_t i1;
    };
    struct {
      uint16_t len;
      uint8_t *data;
    };
  };
};

struct cp_field_info {
  uint16_t aflags;
  uint16_t nameNdx;
  uint16_t descNdx;
};
struct cp_method_info {
  uint16_t aflags;
  uint16_t nameNdx;
  uint16_t descNdx;
};

static uint32_t read32(uint8_t* &buffer) {
  uint32_t rv;
  memcpy(&rv, buffer, sizeof(uint32_t));
  buffer += sizeof(uint32_t);
  return htonl(rv);
}

static uint32_t read16(uint8_t* &buffer) {
  uint16_t rv;
  memcpy(&rv, buffer, sizeof(uint16_t));
  buffer += sizeof(uint16_t);
  return htons(rv);
}
}

#define CP_CONST_UTF8         (1)
#define CP_CONST_INT          (3)
#define CP_CONST_FLOAT        (4)
#define CP_CONST_LONG         (5)
#define CP_CONST_DOUBLE       (6)
#define CP_CONST_CLASS        (7)
#define CP_CONST_STRING       (8)
#define CP_CONST_FIELD        (9)
#define CP_CONST_METHOD      (10)
#define CP_CONST_INTERFACE   (11)
#define CP_CONST_NAMEANDTYPE (12)
#define CP_CONST_METHHANDLE  (15)
#define CP_CONST_METHTYPE    (16)
#define CP_CONST_INVOKEDYN   (18)

static bool parse_cp_entry(uint8_t* &buffer, cp_entry &cpe) {
  cpe.tag = *buffer++;
  switch(cpe.tag) {
  case CP_CONST_CLASS:
  case CP_CONST_STRING:
  case CP_CONST_METHTYPE:
    cpe.s0 = read16(buffer);
    return true;
  case CP_CONST_FIELD:
  case CP_CONST_METHOD:
  case CP_CONST_INTERFACE:
  case CP_CONST_NAMEANDTYPE:
  case CP_CONST_METHHANDLE:
    cpe.s0 = read16(buffer);
    cpe.s1 = read16(buffer);
    return true;
  case CP_CONST_INT:
  case CP_CONST_FLOAT:
    cpe.i0 = read32(buffer);
    return true;
  case CP_CONST_LONG:
  case CP_CONST_DOUBLE:
    cpe.i0 = read32(buffer);
    cpe.i1 = read32(buffer);
    return true;
  case CP_CONST_UTF8:
    cpe.len = read16(buffer);
    cpe.data = buffer;
    buffer += cpe.len;
    return true;
  case CP_CONST_INVOKEDYN:
    fprintf(stderr, "INVOKEDYN constant unsupported, Bailing\n");
    return false;
  }
  fprintf(stderr, "Unrecognized constant pool tag 0x%02x, Bailing\n",
          cpe.tag);
  return false;
}

static void skip_attributes(uint8_t* &buffer) {
  /* Todo:
   * Consider adding some verification so we don't walk
   * off the end in the case of a corrupt class file.
   */
  uint16_t acount = read16(buffer);
  for (int i=0; i<acount; i++) {
    buffer += 2; // Skip name_index
    uint32_t length = read32(buffer);
    buffer += length;
  }
}
#define MAX_CLASS_NAMELEN (4 * 1024)
static DexType *make_dextype_from_cref(std::vector<cp_entry> &cpool,
                                       uint16_t cref) {
  char nbuffer[MAX_CLASS_NAMELEN];
  if (cpool[cref].tag != CP_CONST_CLASS) {
    fprintf(stderr, "Non-class ref in get_class_name, Bailing\n");
    return nullptr;
  }
  uint16_t utf8ref = cpool[cref].s0;
  const cp_entry &utf8cpe = cpool[utf8ref];
  if (utf8cpe.tag != CP_CONST_UTF8) {
    fprintf(stderr, "Non-utf8 ref in get_utf8, Bailing\n");
    return nullptr;
  }
  if (utf8cpe.len > (MAX_CLASS_NAMELEN + 3)) {
    fprintf(stderr, "classname is greater than max, bailing");
    return nullptr;
  }
  nbuffer[0] = 'L';
  memcpy(nbuffer+1, utf8cpe.data, utf8cpe.len);
  nbuffer[1+utf8cpe.len] = ';';
  nbuffer[2+utf8cpe.len] = '\0';
  return DexType::make_type(nbuffer);
}

static bool extract_utf8(std::vector<cp_entry> &cpool, uint16_t utf8ref,
                         char *out, uint32_t size) {
  const cp_entry &utf8cpe = cpool[utf8ref];
  if (utf8cpe.tag != CP_CONST_UTF8) {
    fprintf(stderr, "Non-utf8 ref in get_utf8, Bailing\n");
    return false;
  }
  if (utf8cpe.len > (size - 1)) {
    fprintf(stderr, "name is greater than max, bailing");
    return false;
  }
  memcpy(out, utf8cpe.data, utf8cpe.len);
  out[utf8cpe.len] = '\0';
  return true;
}

static DexField *make_dexfield(std::vector<cp_entry> &cpool,
                               DexType *self, cp_field_info &finfo) {
  char dbuffer[MAX_CLASS_NAMELEN];
  char nbuffer[MAX_CLASS_NAMELEN];
  if (!extract_utf8(cpool, finfo.nameNdx, nbuffer, MAX_CLASS_NAMELEN) ||
     !extract_utf8(cpool, finfo.descNdx, dbuffer, MAX_CLASS_NAMELEN)) {
    return nullptr;
  }
  DexString *name = DexString::make_string(nbuffer);
  DexType *desc = DexType::make_type(dbuffer);
  DexField *field = DexField::make_field(self, name, desc);
  field->set_access((DexAccessFlags)finfo.aflags);
  field->set_external();
  return field;
}

static DexType *simpleTypeB;
static DexType *simpleTypeC;
static DexType *simpleTypeD;
static DexType *simpleTypeF;
static DexType *simpleTypeI;
static DexType *simpleTypeJ;
static DexType *simpleTypeS;
static DexType *simpleTypeZ;
static DexType *simpleTypeV;

static void init_basic_types() {
  simpleTypeB = DexType::make_type("B");
  simpleTypeC = DexType::make_type("C");
  simpleTypeD = DexType::make_type("D");
  simpleTypeF = DexType::make_type("F");
  simpleTypeI = DexType::make_type("I");
  simpleTypeJ = DexType::make_type("J");
  simpleTypeS = DexType::make_type("S");
  simpleTypeZ = DexType::make_type("Z");
  simpleTypeV = DexType::make_type("V");
}

static DexType *parse_type(const char* &buf) {
  char typebuffer[MAX_CLASS_NAMELEN];
  char desc = *buf++;
  switch(desc) {
  case 'B':
    return simpleTypeB;
  case 'C':
    return simpleTypeC;
  case 'D':
    return simpleTypeD;
  case 'F':
    return simpleTypeF;
  case 'I':
    return simpleTypeI;
  case 'J':
    return simpleTypeJ;
  case 'S':
    return simpleTypeS;
  case 'Z':
    return simpleTypeZ;
  case 'V':
    return simpleTypeV;
  case 'L':
    {
      char *tpout = typebuffer;
      *tpout++ = desc;
      while(*buf != ';') {
        *tpout++ = *buf++;
      }
      *tpout++ = *buf++;
      *tpout = '\0';
      return DexType::make_type(typebuffer);
      break;
    }
  case '[':
    {
      char *tpout = typebuffer;
      *tpout++ = desc;
      while(*buf == '[') {
        *tpout++ = *buf++;
      }
      if (*buf == 'L') {
        while(*buf != ';') {
          *tpout++ = *buf++;
        }
        *tpout++ = *buf++;
      } else {
        *tpout++= *buf++;
      }
      *tpout++ = '\0';
      return DexType::make_type(typebuffer);
    }
  }
  fprintf(stderr, "Invalid parse-type '%c', bailing\n", desc);
  return nullptr;
}

static DexTypeList *extract_arguments(const char* &buf) {
  buf++;
  if (*buf == ')') {
    buf++;
    return DexTypeList::make_type_list({});
  }
  std::list<DexType*> args;
  while(*buf != ')') {
    DexType *dtype = parse_type(buf);
    if (dtype == nullptr)
      return nullptr;
    if (dtype == simpleTypeV) {
      fprintf(stderr, "Invalid argument type 'V' in args, bailing\n");
      return nullptr;
    }
    args.push_back(dtype);
  }
  buf++;
  return DexTypeList::make_type_list(std::move(args));
}

static DexMethod *make_dexmethod(std::vector<cp_entry> &cpool,
                               DexType *self, cp_method_info &finfo) {
  char dbuffer[MAX_CLASS_NAMELEN];
  char nbuffer[MAX_CLASS_NAMELEN];
  if (!extract_utf8(cpool, finfo.nameNdx, nbuffer, MAX_CLASS_NAMELEN) ||
     !extract_utf8(cpool, finfo.descNdx, dbuffer, MAX_CLASS_NAMELEN)) {
    return nullptr;
  }
  DexString *name = DexString::make_string(nbuffer);
  const char *ptr = dbuffer;
  DexTypeList *tlist = extract_arguments(ptr);
  if (tlist == nullptr)
    return nullptr;
  DexType *rtype = parse_type(ptr);
  if (rtype == nullptr)
    return nullptr;
  DexProto *proto = DexProto::make_proto(rtype, tlist);
  DexMethod *method = DexMethod::make_method(self, name, proto);
  if (method->is_concrete()) {
    fprintf(stderr, "Pre-concrete method attempted to load '%s', bailing\n", SHOW(method));
    return nullptr;
  }
  uint32_t access = finfo.aflags;
  bool is_virt = true;
  if (nbuffer[0] == '<') {
    is_virt = false;
    if (nbuffer[1] == 'i') {
      access |= ACC_CONSTRUCTOR;
    }
  } else if (access & (ACC_PRIVATE | ACC_STATIC))
    is_virt = false;
  method->set_access((DexAccessFlags)access);
  method->set_virtual(is_virt);
  method->set_external();
  return method;
}

static bool parse_class(uint8_t *buffer) {
  uint32_t magic = read32(buffer);
  uint16_t vminor DEBUG_ONLY = read16(buffer);
  uint16_t vmajor DEBUG_ONLY = read16(buffer);
  uint16_t cp_count = read16(buffer);
  if (magic != kClassMagic) {
    fprintf(stderr, "Bad class magic %08x, Bailing\n", magic);
    return false;
  }
  std::vector<cp_entry> cpool;
  cpool.resize(cp_count);
  /* The zero'th entry is always empty.  Java is annoying. */
  for (int i=1; i<cp_count; i++) {
    if (!parse_cp_entry(buffer, cpool[i]))
      return false;
    if (cpool[i].tag == CP_CONST_LONG ||
       cpool[i].tag == CP_CONST_DOUBLE) {
      cpool[i+1] = cpool[i];
      i++;
    }
  }
  uint16_t aflags = read16(buffer);
  uint16_t clazz = read16(buffer);
  uint16_t super = read16(buffer);
  uint16_t ifcount = read16(buffer);
  DexType *self = make_dextype_from_cref(cpool, clazz);
  if (type_class(self)) {
    return true;
  }
  ClassCreator cc(self);
  cc.set_external();
  if (super != 0) {
    DexType *sclazz = make_dextype_from_cref(cpool, super);
    cc.set_super(sclazz);
  }
  cc.set_access((DexAccessFlags)aflags);
  if (ifcount) {
    for (int i=0; i < ifcount; i++) {
      uint16_t iface = read16(buffer);
      DexType *iftype = make_dextype_from_cref(cpool, iface);
      cc.add_interface(iftype);
    }
  }
  uint16_t fcount = read16(buffer);

  for (int i=0; i < fcount; i++) {
    cp_field_info cpfield;
    cpfield.aflags = read16(buffer);
    cpfield.nameNdx = read16(buffer);
    cpfield.descNdx = read16(buffer);
    skip_attributes(buffer);
    DexField *field = make_dexfield(cpool, self, cpfield);
    if (field == nullptr)
      return false;
    cc.add_field(field);
  }

  uint16_t mcount = read16(buffer);
  if (mcount) {
    for (int i=0; i < mcount; i++) {
      cp_method_info cpmethod;
      cpmethod.aflags = read16(buffer);
      cpmethod.nameNdx = read16(buffer);
      cpmethod.descNdx = read16(buffer);
      skip_attributes(buffer);
      DexMethod *method = make_dexmethod(cpool, self, cpmethod);
      if (method == nullptr)
        return false;
      cc.add_method(method);
    }
  }
  DexClass *dc DEBUG_ONLY = cc.create();
  //#define DEBUG_PRINT
#ifdef DEBUG_PRINT
  fprintf(stderr, "DexClass constructed from jar:\n%s\n", SHOW(dc));
  if (dc->get_sfields().size()) {
    fprintf(stderr, "Static Fields:\n");
    for (auto const& field : dc->get_sfields()) {
      fprintf(stderr, "\t%s\n", SHOW(field));
    }
  }
  if (dc->get_ifields().size()) {
    fprintf(stderr, "Instance Fields:\n");
    for (auto const& field : dc->get_ifields()) {
      fprintf(stderr, "\t%s\n", SHOW(field));
    }
  }
  if (dc->get_dmethods().size()) {
    fprintf(stderr, "Direct Methods:\n");
    for (auto const& method : dc->get_dmethods()) {
      fprintf(stderr, "\t%s\n", SHOW(method));
    }
  }
  if (dc->get_vmethods().size()) {
    fprintf(stderr, "Virtual Methods:\n");
    for (auto const& method : dc->get_vmethods()) {
      fprintf(stderr, "\t%s\n", SHOW(method));
    }
  }

#endif
  return true;
}

/******************
 * Begin Jar Loading code.
 *
 */

namespace {
static const int kSignatureSize = 4;

/* CDFile
 * Central directory file header entry structures.
 */
static const uint16_t kCompMethodDeflate (8);
static const uint8_t kCDFile[] = {'P', 'K', 0x01, 0x02};

struct __attribute__((packed)) pk_cd_file {
  uint32_t signature;
  uint16_t vmade;
  uint16_t vextract;
  uint16_t flags;
  uint16_t comp_method;
  uint16_t mod_time;
  uint16_t mod_date;
  uint32_t crc32;
  uint32_t comp_size;
  uint32_t ucomp_size;
  uint16_t fname_len;
  uint16_t extra_len;
  uint16_t comment_len;
  uint16_t diskno;
  uint16_t interal_attr;
  uint32_t external_attr;
  uint32_t disk_offset;
};

/* CDirEnd:
 * End of central directory record structures.
 */
static const int kMaxCDirEndSearch = 100;
static const uint8_t kCDirEnd[] = {'P', 'K', 0x05, 0x06};

struct __attribute__((packed)) pk_cdir_end {
  uint32_t signature;
  uint16_t diskno;
  uint16_t cd_diskno;
  uint16_t cd_disk_entries;
  uint16_t cd_entries;
  uint32_t cd_size;
  uint32_t cd_disk_offset;
  uint16_t comment_len;
};

/* LFile:
 * Local file header structures.
 * (Yes, this made more sense in the world of floppies and tapes.)
 */
static const uint8_t kLFile[] = {'P', 'K', 0x03, 0x04};

struct __attribute__((packed)) pk_lfile {
  uint32_t signature;
  uint16_t vextract;
  uint16_t flags;
  uint16_t comp_method;
  uint16_t mod_time;
  uint16_t mod_date;
  uint32_t crc32;
  uint32_t comp_size;
  uint32_t ucomp_size;
  uint16_t fname_len;
  uint16_t extra_len;
};

struct jar_entry {
  struct pk_cd_file cd_entry;
  uint8_t *filename;
  ~jar_entry() {
    if (filename != nullptr) {
      free(filename);
      filename = nullptr;
    }
  }
};
}

static bool find_central_directory(uint8_t *mapping, ssize_t size,
                                   pk_cdir_end &pce) {
  ssize_t soffset = (size - sizeof(pk_cdir_end));
  ssize_t eoffset = soffset - kMaxCDirEndSearch;
  if (soffset < 0)
    return false;
  if (eoffset < 0)
    eoffset = 0;
  do {
    uint8_t *cdsearch = mapping + soffset;
    if (memcmp(cdsearch, kCDirEnd, kSignatureSize) == 0) {
      memcpy(&pce, cdsearch, sizeof(pk_cdir_end));
      return true;
    }
  } while(soffset-- > eoffset);
  fprintf(stderr, "End of central directory record not found, bailing\n");
  return false;
}

static bool validate_pce(pk_cdir_end &pce, ssize_t size) {
  /* We only support a limited feature set.  We
   * don't support disk-spanning, so bail if that's the case.
   */
  if (pce.cd_diskno != pce.diskno ||
      pce.cd_diskno != 0 ||
      pce.cd_entries != pce.cd_disk_entries) {
    fprintf(stderr, "Disk spanning is not supported, bailing\n");
    return false;
  }
  ssize_t data_size = size - sizeof(pk_cdir_end);
  if (pce.cd_disk_offset + pce.cd_size > data_size) {
    fprintf(stderr, "Central directory overflow, invalid pce structure\n");
    return false;
  }
  return true;
}

static bool extract_jar_entry(uint8_t* &mapping, jar_entry &je) {
  if (memcmp(mapping, kCDFile, kSignatureSize) != 0) {
    fprintf(stderr, "Invalid central directory entry, bailing\n");
    return false;
  }
  memcpy(&je.cd_entry, mapping, sizeof(pk_cd_file));
  mapping += sizeof(pk_cd_file);
  je.filename = (uint8_t*)malloc(je.cd_entry.fname_len + 1);
  memcpy(je.filename, mapping, je.cd_entry.fname_len);
  je.filename[je.cd_entry.fname_len] = '\0';
  mapping += je.cd_entry.fname_len;
  mapping += je.cd_entry.extra_len;
  mapping += je.cd_entry.comment_len;
  return true;
}

static bool get_jar_entries(uint8_t *mapping, pk_cdir_end &pce,
                            std::vector<jar_entry> &files) {
  uint8_t *cdir = mapping + pce.cd_disk_offset;
  files.resize(pce.cd_entries);
  for (int entry=0; entry < pce.cd_entries; entry++) {
    if (!extract_jar_entry(cdir, files[entry]))
      return false;
  }
  return true;
}

static int jar_uncompress(Bytef *dest, uLongf *destLen, const Bytef *source,
                   uLong sourceLen) {
  z_stream stream;
  int err;

  stream.next_in = (Bytef *)source;
  stream.avail_in = (uInt)sourceLen;
  stream.next_out = dest;
  stream.avail_out = (uInt)*destLen;
  stream.zalloc = (alloc_func)0;
  stream.zfree = (free_func)0;

  err = inflateInit2(&stream, -MAX_WBITS);
  if (err != Z_OK) return err;

  err = inflate(&stream, Z_FINISH);
  if (err != Z_STREAM_END) {
    inflateEnd(&stream);
    return err;
  }
  *destLen = stream.total_out;

  err = inflateEnd(&stream);
  return err;
}

static bool decompress_class(jar_entry &file, uint8_t *mapping,
                             uint8_t *outbuffer, int bufsize) {
  if (file.cd_entry.comp_method != kCompMethodDeflate) {
    fprintf(stderr, "Unknown compression method %d, Bailing\n",
            file.cd_entry.comp_method);
    return false;
  }
  uint8_t *lfile = mapping + file.cd_entry.disk_offset;
  if (memcmp(lfile, kLFile, kSignatureSize) != 0) {
    fprintf(stderr, "Invalid local file entry, bailing\n");
    return false;
  }
  pk_lfile pkf;
  memcpy(&pkf, lfile, sizeof(pk_lfile));
  if (pkf.comp_size == 0 && pkf.ucomp_size == 0 &&
     pkf.comp_size != file.cd_entry.comp_size &&
     pkf.ucomp_size != file.cd_entry.ucomp_size) {
    pkf.comp_size = file.cd_entry.comp_size;
    pkf.ucomp_size = file.cd_entry.ucomp_size;
  }
  lfile += sizeof(pk_lfile);
  if (pkf.fname_len != file.cd_entry.fname_len ||
     pkf.comp_size != file.cd_entry.comp_size ||
     pkf.ucomp_size != file.cd_entry.ucomp_size ||
     pkf.comp_method != file.cd_entry.comp_method ||
     memcmp(lfile, file.filename, pkf.fname_len) != 0) {
    fprintf(stderr, "Directory entry doesn't match local file header, "
            "Bailing %d %d %d %d, %d %d %d %d extra %d\n",
            pkf.fname_len, pkf.comp_size, pkf.ucomp_size, pkf.comp_method,
            file.cd_entry.fname_len, file.cd_entry.comp_size, file.cd_entry.ucomp_size, file.cd_entry.comp_method, pkf.extra_len);
    return false;
  }
  lfile += pkf.fname_len;
  lfile += pkf.extra_len;
  uLongf dlen = bufsize;
  int zlibrv = jar_uncompress(outbuffer, &dlen, lfile, pkf.comp_size);
  if (zlibrv != Z_OK) {
    fprintf(stderr, "uncompress failed with code %d, Bailing\n", zlibrv);
    return false;
  }
  if (dlen != pkf.ucomp_size) {
    fprintf(stderr, "mis-match on uncompressed size, Bailing\n");
    return false;
  }
  return true;
}

static const int kStartBufferSize = 128 * 1024;

static bool process_jar_entries(std::vector<jar_entry> &files,
                                uint8_t *mapping) {
  ssize_t bufsize = kStartBufferSize;
  uint8_t *outbuffer = (uint8_t*)malloc(bufsize);
  static char classEndString[] = ".class";
  static int classEndStringLen = strlen(classEndString);
  init_basic_types();
  for (auto &file : files) {
    if (file.cd_entry.ucomp_size == 0)
      continue;
    if (file.cd_entry.fname_len < (classEndStringLen  + 1))
      continue;

    // Skip non-class files
    uint8_t *endcomp = file.filename +
      (file.cd_entry.fname_len - classEndStringLen);
    if (memcmp(endcomp, classEndString, classEndStringLen) != 0)
      continue;

    // Resize output if necessary.
    if (bufsize < file.cd_entry.ucomp_size) {
      while(bufsize < file.cd_entry.ucomp_size)
        bufsize *= 2;
      free(outbuffer);
      outbuffer = (uint8_t*)malloc(bufsize);
    }

    if (!decompress_class(file, mapping, outbuffer, bufsize)) {
      free(outbuffer);
      return false;
    }

    if (!parse_class(outbuffer)) {
      free(outbuffer);
      return false;
    }
  }
  free(outbuffer);
  return true;
}

static bool process_jar(uint8_t *mapping, ssize_t size) {
  pk_cdir_end pce;
  std::vector<jar_entry> files;
  if (!find_central_directory(mapping, size, pce))
    return false;
  if (!validate_pce(pce, size))
    return false;
  if (!get_jar_entries(mapping, pce, files))
    return false;
  if (!process_jar_entries(files, mapping)) {
    return false;
  }
  return true;
}

bool load_jar_file(const char *location) {
  int fd = open(location, O_RDONLY);
  struct stat stat;
  ssize_t size;
  uint8_t *mapping;
  if (fd < 0) {
    fprintf(stderr, "Cannot open jar file %s\n", location);
    return false;
  }
  if (fstat(fd, &stat)) {
    fprintf(stderr, "Cannot fstat file %s\n", location);
    return false;
  }
  size = stat.st_size;
  mapping = (uint8_t*)mmap(nullptr, size, PROT_READ,
                         MAP_FILE | MAP_SHARED, fd, 0);
  close(fd);
  if (mapping == MAP_FAILED) {
    mapping = nullptr;
    perror("Address space allocation failed for mmap\n");
    return false;
  }
  bool rv = process_jar(mapping, size);
  munmap(mapping, size);
  return rv;
}

//#define LOCAL_MAIN
#ifdef LOCAL_MAIN
int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "You must specify a jar file\n");
    return -1;
  }
  for (int jarno=1; jarno < argc; jarno++) {
    if (!load_jar_file(argv[jarno])) {
      fprintf(stderr, "Failed to load jar %s, bailing\n", argv[jarno]);
      return -2;
    }
  }
}
#endif