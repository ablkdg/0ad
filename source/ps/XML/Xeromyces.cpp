#include "precompiled.h"

#include <vector>
#include <set>
#include <map>
#include <stack>
#include <algorithm>

#include "ps/CLogger.h"
#include "lib/res/file/vfs.h"
#include "Xeromyces.h"

#define ZLIB_DLL
#include <zlib.h> // for crc32

#define LOG_CATEGORY "xml"

#include "XML.h"


int CXeromyces::XercesLoaded = 0; // for once-only initialisation


// Slightly nasty fwrite/fseek/ftell style thing
class membuffer
{
public:
	membuffer()
	{
		buffer = (char*)malloc(bufferinc);
		debug_assert(buffer);
		allocated = bufferinc;
		length = 0;
	}

	~membuffer()
	{
		free(buffer);
	}

	void write(const void* data, int size)
	{
		while (length + size >= allocated) grow();
		memcpy2(&buffer[length], data, size);
		length += size;
	}

	void write(const void* data, int size, int offset)
	{
		debug_assert(offset >= 0 && offset+size < length);
		memcpy2(&buffer[offset], data, size);
	}

	int tell()
	{
		return length;
	}

	char* steal_buffer()
	{
		char* ret = buffer;
		buffer = NULL;
		return ret;
	}

	char* buffer;
	int length;
private:
	int allocated;
	static const int bufferinc = 1024;
	void grow()
	{
		allocated += bufferinc;
		char* newbuffer = (char*)realloc(buffer, allocated);
		if (newbuffer)
			buffer = newbuffer;
		else
			debug_warn("Xeromyces: realloc failed");
	}
};

// Convenient storage for the internal tree
typedef struct {
	std::string name;
	utf16string value;
} XMLAttribute;

typedef struct XMLElement {
	std::string name;
	int linenum;
	utf16string text;
	std::vector<XMLElement*> childs;
	std::vector<XMLAttribute*> attrs;
} XMLElement;

class XeroHandler : public DefaultHandler
{
public:
	XeroHandler() : m_locator(NULL), Root(NULL) {}
	~XeroHandler()
	{
		if (Root)
			DeallocateElement(Root);
	}

	// SAX2 event handlers:
	virtual void startDocument();
	virtual void endDocument();
	virtual void startElement(const XMLCh* const uri, const XMLCh* const localname, const XMLCh* const qname, const Attributes& attrs);
	virtual void endElement(const XMLCh* const uri, const XMLCh* const localname, const XMLCh* const qname);
	virtual void characters(const XMLCh* const chars, const unsigned int length);
	
	const Locator* m_locator;

	virtual void setDocumentLocator(const Locator* const locator)
	{
		m_locator = locator;
	}

	// Non-SAX2 stuff, used for storing the
	// parsed data and constructing the XMB:
	
	void CreateXMB();
	membuffer buffer;

private:
	std::set<std::string> ElementNames;
	std::set<std::string> AttributeNames;
	XMLElement* Root;
	XMLElement* CurrentElement;
	std::stack<XMLElement*> ElementStack;

	std::map<std::string, int> ElementID;
	std::map<std::string, int> AttributeID;

	void OutputElement(XMLElement* el);

	// Recursively frees memory
	void DeallocateElement(XMLElement* el);
};



CXeromyces::CXeromyces()
: XMBFileHandle(0), XMBBuffer(NULL)
{
}

CXeromyces::~CXeromyces() {

	if (XMBFileHandle)
	{
		// If it was read from a file, close it
		delete XMBFileHandle;
	}
	else
	{
		// If it was converted from a XML directly into memory,
		// free that memory buffer
		free(XMBBuffer);
	}
}

void CXeromyces::Terminate()
{
	if (XercesLoaded)
	{
		XMLPlatformUtils::Terminate();
		XercesLoaded = 0;
	}
}


// Find out write location of the XMB file corresponding to xmlFilename
void CXeromyces::getXMBPath(const char* xmlFilename, const char* xmbFilename,
	char* xmbPath)
{
	// rationale:
	// - it is necessary to write out XMB files into a subdirectory
	//   corresponding to the mod from which the XML file is taken.
	//   this avoids confusion when multiple mods are active -
	//   their XMB files' VFS filename would otherwise be indistinguishable.
	// - we group files in the cache/ mount point first by mod, and only
	//   then XMB. this is so that all output files for a given mod can
	//   easily be deleted. the operation of deleting all old/unused
	//   XMB files requires a program anyway (to find out which are no
	//   longer needed), so it's not a problem that XMB files reside in
	//   a subdirectory (which would make manually deleting all harder).

	// get real path of XML file (e.g. mods/official/entities/...)
	char P_XMBRealPath[PATH_MAX];
	vfs_realpath(xmlFilename, P_XMBRealPath);

	// extract mod name from that
	char modName[PATH_MAX];
	// .. NOTE: can't use %s, of course (keeps going beyond '/')
	int matches = sscanf(P_XMBRealPath, "mods/%[^/]", modName);
	debug_assert(matches == 1);

	// build full name: cache, then mod name, XMB subdir, original XMB path
	snprintf(xmbPath, PATH_MAX, "cache/mods/%s/xmb/%s", modName, xmbFilename);
}

PSRETURN CXeromyces::Load(const char* filename)
{
	// Make sure the .xml actually exists
	if (! vfs_exists(filename))
	{
		LOG(ERROR, LOG_CATEGORY, "CXeromyces: Failed to find XML file %s", filename);
		return PSRETURN_Xeromyces_XMLOpenFailed;
	}

	// Get some data about the .xml file
	struct stat xmlStat;
	if (vfs_stat(filename, &xmlStat) < 0)
	{
		LOG(ERROR, LOG_CATEGORY, "CXeromyces: Failed to stat XML file %s", filename);
		return PSRETURN_Xeromyces_XMLOpenFailed;
	}


	/*
	XMBs are stored with a unique name, where the name is generated from
	characteristics of the XML file. If a file already exists with the
	generated name, it is assumed that that file is a valid conversion of
	the XML, and so it's loaded. Otherwise, the XMB is created with that
	filename.

	This means it's never necessary to overwrite existing XMB files; since
	the XMBs are often in archives, it's not easy to rewrite those files,
	and it's not possible to switch to using a loose file because the VFS
	has already decided that file is inside an archive. So each XMB is given
	a unique name, and old ones are somehow purged.
	*/


	// Generate the filename for the xmb:
	//     <xml filename>_<mtime><size><format version>.xmb
	// with mtime/size as 8-digit hex, where mtime's lowest bit is
	// zeroed because zip files only have 2 second resolution.

	CStr xmbFilename = filename;

	// Strip the .xml suffix
	int pos;
	if ((pos = xmbFilename.FindInsensitive(".xml")) != -1)
		xmbFilename = xmbFilename.Left(pos);

	const int bufLen = 22;
	char buf[bufLen+1];
	if (sprintf(buf, "_%08x%08xB.xmb", (int)xmlStat.st_mtime & ~1, (int)xmlStat.st_size) != bufLen)
	{
		debug_warn("Failed to create filename (?!)");
		return PSRETURN_Xeromyces_XMLOpenFailed;
	}
	xmbFilename += buf;

	char xmbPath[PATH_MAX];
	getXMBPath(filename, xmbFilename, xmbPath);


	// If the file exists, use it
	if (vfs_exists(xmbPath))
	{
		if (ReadXMBFile(xmbPath))
			return PSRETURN_OK;
		else
			return PSRETURN_Xeromyces_XMLOpenFailed;
	}

	
	// XMB isn't up to date with the XML, so rebuild it:

	// Load Xerces if necessary
	if (! XercesLoaded)
	{
		XMLPlatformUtils::Initialize();
		XercesLoaded = 1;
	}

	// Open the .xml file
	// note: FILE_LONG_LIVED is necessary because we load XML, load DTD,
	// and only then free XML.
	CVFSInputSource source;
	if (source.OpenFile(filename, FILE_LONG_LIVED) < 0)
	{
		LOG(ERROR, LOG_CATEGORY, "CXeromyces: Failed to open XML file %s", filename);
		return PSRETURN_Xeromyces_XMLOpenFailed;
	}

	// Set up the Xerces parser
	SAX2XMLReader* Parser = XMLReaderFactory::createXMLReader();

	// Enable validation
	Parser->setFeature(XMLUni::fgSAX2CoreValidation, true);
	Parser->setFeature(XMLUni::fgXercesDynamic, true);

	XeroHandler handler;
	Parser->setContentHandler(&handler);

	CXercesErrorHandler errorHandler;
	Parser->setErrorHandler(&errorHandler);

	CVFSEntityResolver entityResolver(filename);
	Parser->setEntityResolver(&entityResolver);

	// Build a tree inside handler
	Parser->parse(source);

	// (It's horribly inefficient doing SAX2->tree then tree->XMB,
	// but the XML->XMB conversion should be done very rarely
	// anyway. If it's ever needed, the XMB writing can be done
	// directly from inside the SAX2 event handlers, although that's
	// a little more complex)

	delete Parser;

	if (errorHandler.getSawErrors())
	{
		LOG(ERROR, LOG_CATEGORY, "CXeromyces: Errors in XML file '%s'", filename);
		return PSRETURN_Xeromyces_XMLParseError;
		// The internal tree of the XeroHandler will be cleaned up automatically
	}

	// Convert the data structures into the XMB format
	handler.CreateXMB();

	// Save the file to disk, so it can be loaded quickly next time
	vfs_store(xmbPath, handler.buffer.buffer, handler.buffer.length, FILE_NO_AIO);

	// Store the buffer so it can be freed later
	XMBBuffer = handler.buffer.steal_buffer();

	// Set up the XMBFile
	Initialise(XMBBuffer);

	return PSRETURN_OK;
}

bool CXeromyces::ReadXMBFile(const char* filename)
{
	CVFSFile* file = new CVFSFile;
	// note: an XMB file's buffer is held in memory across all load/free
	// sequences of dependent files it references. that hurts the
	// file cache allocator and incurs a warning unless we
	// inform the file manager of this behavior via FILE_LONG_LIVED.
	if (file->Load(filename, FILE_LONG_LIVED) != PSRETURN_OK)
		return false;

	const void* buffer = file->GetBuffer();

	debug_assert(file->GetBufferSize() >= 42 && "Invalid XMB file"); // 42 bytes is the smallest possible XMB. (Well, maybe not quite, but it's a nice number.)
	debug_assert(*(u32*)buffer == HeaderMagic && "Invalid XMB file header");

	// Store the Handle so it can be closed later
	XMBFileHandle = file;

	// Set up the XMBFile
	Initialise((const char*)buffer);

	return true;
}



void XeroHandler::startDocument()
{
	Root = new XMLElement;
	ElementStack.push(Root);
}

void XeroHandler::endDocument()
{
}

/*
// Silently clobbers non-ASCII characters
std::string lowercase_ascii(const XMLCh *a)
{
	std::string b;
	uint len=XMLString::stringLen(a);
	b.resize(len);
	for (uint i = 0; i < len; ++i)
		b[i] = (char)towlower(a[i]);
	return b;
}
*/

/**
 * Return an ASCII version of the given 16-bit string, ignoring
 * any non-ASCII characters.
 *
 * @param const XMLCh * a Input string.
 * @return std::string 8-bit ASCII version of <code>a</code>.
 **/
std::string toAscii( const XMLCh* a )
{
	std::string b;
	uint len=XMLString::stringLen(a);
	b.reserve(len);
	for (uint i = 0; i < len; ++i)
	{
		if(iswascii(a[i]))
			b += (char) a[i];
	}
	return b;
}

void XeroHandler::startElement(const XMLCh* const UNUSED(uri), const XMLCh* const localname, const XMLCh* const UNUSED(qname), const Attributes& attrs)
{
	std::string elementName = toAscii(localname);
	ElementNames.insert(elementName);

	// Create a new element
	XMLElement* e = new XMLElement;
	e->name = elementName;
	e->linenum = m_locator->getLineNumber();

	// Store all the attributes in the new element
	for (unsigned int i = 0; i < attrs.getLength(); ++i)
	{
		std::string attrName = toAscii(attrs.getLocalName(i));
		AttributeNames.insert(attrName);
		XMLAttribute* a = new XMLAttribute;
		a->name = attrName;
		const XMLCh *tmp = attrs.getValue(i);
		a->value = utf16string(tmp, tmp+XMLString::stringLen(tmp));
		e->attrs.push_back(a);
	}

	// Add the element to its parent
	ElementStack.top()->childs.push_back(e);

	// Set as parent of following elements
	ElementStack.push(e);
}

void XeroHandler::endElement(const XMLCh* const UNUSED(uri), const XMLCh* const UNUSED(localname), const XMLCh* const UNUSED(qname))
{
	ElementStack.pop();
}

void XeroHandler::characters(const XMLCh* const chars, const unsigned int UNUSED(length))
{
	ElementStack.top()->text += utf16string(chars, chars+XMLString::stringLen(chars));
}


void XeroHandler::CreateXMB()
{
	// Header
	buffer.write((void*)HeaderMagicStr, 4);

	std::set<std::string>::iterator it;
	int i;

	// Element names
	i = 0;
	int ElementCount = (int)ElementNames.size();
	buffer.write(&ElementCount, 4);
	for (it = ElementNames.begin(); it != ElementNames.end(); ++it)
	{
		int TextLen = (int)it->length()+1;
		buffer.write(&TextLen, 4);
		buffer.write((void*)it->c_str(), TextLen);
		ElementID[*it] = i++;
	}

	// Attribute names
	i = 0;
	int AttributeCount = (int)AttributeNames.size();
	buffer.write(&AttributeCount, 4);
	for (it = AttributeNames.begin(); it != AttributeNames.end(); ++it)
	{
		int TextLen = (int)it->length()+1;
		buffer.write(&TextLen, 4);
		buffer.write((void*)it->c_str(), TextLen);
		AttributeID[*it] = i++;
	}

	// All the XML contents must be surrounded by a single element
	debug_assert(Root->childs.size() == 1);

	OutputElement(Root->childs[0]);

	delete Root;
	Root = NULL;
}

// Writes a whole element (recursively if it has children) into the buffer,
// and also frees all the memory that has been allocated for that element.
void XeroHandler::OutputElement(XMLElement* el)
{
	// Filled in later with the length of the element
	int Pos_Length = buffer.tell();
	buffer.write("????", 4);

	int NameID = ElementID[el->name];
	buffer.write(&NameID, 4);

	int AttrCount = (int)el->attrs.size();
	buffer.write(&AttrCount, 4);

	int ChildCount = (int)el->childs.size();
	buffer.write(&ChildCount, 4);

	// Filled in later with the offset to the list of child elements
	int Pos_ChildrenOffset = buffer.tell();
	buffer.write("????", 4);


	// Trim excess whitespace in the entity's text, while counting
	// the number of newlines trimmed (so that JS error reporting
	// can give the correct line number)

	std::string whitespaceA = " \t\r\n";
	utf16string whitespace (whitespaceA.begin(), whitespaceA.end());

	// Find the start of the non-whitespace section
	size_t first = el->text.find_first_not_of(whitespace);

	if (first == el->text.npos)
		// Entirely whitespace - easy to handle
		el->text = utf16string();

	else
	{
		// Count the number of \n being cut off,
		// and add them to the line number
		utf16string trimmed (el->text.begin(), el->text.begin()+first);
		el->linenum += (int)std::count(trimmed.begin(), trimmed.end(), (utf16_t)'\n');

		// Find the end of the non-whitespace section,
		// and trim off everything else
		size_t last = el->text.find_last_not_of(whitespace);
		el->text = el->text.substr(first, 1+last-first);
	}

	// Output text, prefixed by length in bytes
	if (el->text.length() == 0)
	{
		// No text; don't write much
		buffer.write("\0\0\0\0", 4);
	}
	else
	{
		// Write length and line number and null-terminated text
		int NodeLen = 4 + 2*((int)el->text.length()+1);
		buffer.write(&NodeLen, 4);
		buffer.write(&el->linenum, 4);
		buffer.write((void*)el->text.c_str(), NodeLen-4);
	}

	// Output attributes

	int i;

	for (i = 0; i < AttrCount; ++i)
	{
		int AttrName = AttributeID[el->attrs[i]->name];
		buffer.write(&AttrName, 4);

		int AttrLen = 2*((int)el->attrs[i]->value.length()+1);
		buffer.write(&AttrLen, 4);
		buffer.write((void*)el->attrs[i]->value.c_str(), AttrLen);

		// Free each attribute as soon as it's been dealt with
		delete el->attrs[i];
	}

	// Go back and fill in the child-element offset
	int ChildrenOffset = buffer.tell() - (Pos_ChildrenOffset+4);
	buffer.write(&ChildrenOffset, 4, Pos_ChildrenOffset);

	// Output all child nodes
	for (i = 0; i < ChildCount; ++i)
		OutputElement(el->childs[i]);

	// Go back and fill in the length
	int Length = buffer.tell() - Pos_Length;
	buffer.write(&Length, 4, Pos_Length);

	// Tidy up the parser's mess
	delete el;
}

void XeroHandler::DeallocateElement(XMLElement* el)
{
	size_t i;

	for (i = 0; i < el->attrs.size(); ++i)
		delete el->attrs[i];

	for (i = 0; i < el->childs.size(); ++i)
		DeallocateElement(el->childs[i]);

	delete el;
}
