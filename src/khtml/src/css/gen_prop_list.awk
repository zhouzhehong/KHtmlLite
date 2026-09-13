BEGIN { n=0 }
/^[[:space:]]*#/ { next }
/^[[:space:]]*$/ { next }
{ n++; vals[n]=$0 }
END {
  print ""
  print "static const char * const propertyList[] = {"
  print "    nullptr,"
  for(i=1;i<=n;i++) print "    \"" vals[i] "\","
  print "    nullptr"
  print "};"
  print "DOMString getPropertyName(unsigned short id)"
  print "{"
  print "    if(id >= CSS_PROP_TOTAL || id == 0)"
  print "      return DOMString();"
  print "    else"
  print "      return DOMString(propertyList[id]);"
  print "}"
}
