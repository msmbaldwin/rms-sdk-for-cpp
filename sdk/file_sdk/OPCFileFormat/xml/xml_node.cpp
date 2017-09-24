#include "OPCFileFormat/xml/xml_node.h"

#ifdef __cplusplus
extern "C"
{
#endif // __cplusplus
#include "libxml/globals.h"
#ifdef __cplusplus
}
#endif // __cplusplus

#include "OPCFileFormat/xml/xml_helper.h"

using std::string;

namespace mip {

string XmlNode::GetAttributeValue(const string& attributeName) const {
  if (!mNode)
    return string();

  const auto attributeValue = UniquePtr<xmlChar>(xmlGetProp(mNode, ConvertXmlString(attributeName)), DeleteXmlString);
  return attributeValue ? ConvertXmlString(attributeValue.get()) : string();
}

XmlNode XmlNode::GetNextNode() const {
  return XmlNode(mNode ? mNode->next : nullptr);
}

XmlNode XmlNode::GetFirstChild() const {
  return XmlNode(mNode ? mNode->children : nullptr);
}

string XmlNode::GetNodeName() const {
  if (mNode && mNode->name)
    return ConvertXmlString(mNode->name);
  return string();
}

string XmlNode::GetNodeInnerText() const {
  if (mNode && mNode->children && mNode->children->type == XML_TEXT_NODE && mNode->children->content)
    return ConvertXmlString(mNode->children->content);
  return string();
}

string XmlNode::GetNodeNamespace() const {
  if (mNode && mNode->ns && mNode->ns->prefix)
    return ConvertXmlString(mNode->ns->prefix);
  return string();
}

}  // namespace mip