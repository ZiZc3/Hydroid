#ifndef IMANGI_XLS_H
#define IMANGI_XLS_H

// Imangi's ImangiXLSHelper (imangi_xls.c): malloc'd text, or NULL as Java's catch gives
char *imangi_xls_parse(const char *text);                   // DoParse: one config file
char *imangi_xls_parse_list(const char *text, int *count);  // ParseList: a JSON array of them

#endif
