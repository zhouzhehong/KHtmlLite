BEGIN { n=0 }
/^[[:space:]]*#/ { next }
/^[[:space:]]*$/ { next }
{ n++; vals[n]=$0 }
END {
  print "%{"
  print "#include \"cssproperties.h\""
  print "struct css_prop { const char *name; int id; };"
  print "%}"
  print "struct css_prop;"
  print "%%"
  for(i=1;i<=n;i++) print vals[i] ", " i
  print "%%"
}
