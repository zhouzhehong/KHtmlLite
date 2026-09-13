BEGIN { n=0 }
/^[[:space:]]*#/ { next }
/^[[:space:]]*$/ { next }
{ n++; vals[n]=$0 }
END {
  print "%{"
  print "#include \"cssvalues.h\""
  print "struct css_value { const char *name; int id; };"
  print "%}"
  print "struct css_value;"
  print "%%"
  for(i=1;i<=n;i++) print vals[i] ", " i
  print "%%"
}
