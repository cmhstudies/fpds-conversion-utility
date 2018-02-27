/*
 * FPDS Archive Conversion Utility
 *
 * Copyright (c) 2018 William Muir
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <getopt.h> 
#include <libxml/xmlreader.h>
#include <libxml/xmlstring.h>
#include <libxslt/transform.h>
#include <libxslt/xsltutils.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

#include "create-table.h"
#include "create-view-document-id.h"
#include "create-view-fact.h"
#include "insert-row.h"
#include "normalize-record.h"

static int aflg, oflg, hflg;
static char *xml_archive, *sqlite3_target;
static sqlite3 *db;
static xsltStylesheetPtr create_table, insert_row, normalize_record;
static void usage(void);
static void cleanup(void);
static xmlXPathObjectPtr getXPath (xmlDocPtr doc, xmlChar *xpath);
static void buildTable (xmlDocPtr parsedTableXML);
static void insertRecord (xmlDocPtr parsed_table_xml, int record_number);
static xmlDocPtr normalizeXML(xmlTextReaderPtr reader);
static void streamFile(const char *filename, int record_number);
static void writeSQL (xmlDocPtr norm_xml, int record_number);


/**
 * usage:
 * 
 * Print command line usage
 */
static void
usage() {
    fprintf(stderr, "%s\n",
        "usage: conversion-utility [-a | -o] xml_archive sqlite3_target");
}


/**
 * cleanup:
 * 
 * Memory management prior to program exit
 */
static void
cleanup(void) {
    xsltFreeStylesheet(normalize_record);
    xsltFreeStylesheet(create_table);
    xsltFreeStylesheet(insert_row);
    xsltCleanupGlobals();
    xmlCleanupParser();
}


/**
 * getXPath:
 * @doc: xml
 * @xpath: xpath syntax 
 * 
 * Return xpath object
 */
static xmlXPathObjectPtr
getXPath (xmlDocPtr doc, xmlChar *xpath) {
    xmlXPathContextPtr context;
    xmlXPathObjectPtr result;

    context = xmlXPathNewContext(doc);
    result = xmlXPathEvalExpression(xpath, context);
    xmlXPathFreeContext(context);

    return result;
}


/**
 * buildTable:
 * @parsed_table_xml: xml for the table
 * 
 * Create a table (if not exists) in the database
 */
static void
buildTable (xmlDocPtr parsed_table_xml) {
    char *err_msg;
    int buffersize, rc;
    xmlDocPtr result;
    xmlChar *sql_text;

    /* Use the `create_table' xsl stylesheet to generate an SQL statement */
    result = xsltApplyStylesheet(create_table, parsed_table_xml, NULL);
    xsltSaveResultToString(&sql_text, &buffersize, result, create_table);
    xmlFreeDoc(result);

    /* Execute the statement */
    rc = sqlite3_exec(db, (char *)sql_text, 0, 0, &err_msg);
    if (rc != SQLITE_OK)
        printf("ERROR creating table: %s\n", sqlite3_errmsg(db));

    sqlite3_free(err_msg);
    xmlFree(sql_text);
}


/**
 * createViews:
  * 
 * Create views in the database
 */
static void
createViews () {
    char *err_msg;
    int rc;
    
    rc = sqlite3_exec(db, (char *)create_view_document_id_sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK)
        printf("ERROR creating document view: %s\n", sqlite3_errmsg(db));

    rc = sqlite3_exec(db, (char *)create_view_fact_sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK)
        printf("ERROR creating fact view: %s\n", sqlite3_errmsg(db));

    sqlite3_free(err_msg);
}


/**
 * insertRecord:
 * @parsed_table_xml: xml for the table
 * @record_number: id field for database
 * 
 * Insert a record into the database
 */
static void
insertRecord (xmlDocPtr parsed_table_xml, int record_number) {
    int buffersize, i, rowdata;
    sqlite3_stmt *stmt;
    xmlChar *sql_text;
    xmlDocPtr result;
    xmlNodeSetPtr column_nodeset;
    xmlXPathObjectPtr columns;

    /* Store a prepared statement for inserting the record */
    result = xsltApplyStylesheet(insert_row, parsed_table_xml, NULL);
    xsltSaveResultToString(&sql_text, &buffersize, result, insert_row);
    xmlFreeDoc(result);

    /* Use XPath to find the columns (fields) */
    columns = getXPath(parsed_table_xml, (xmlChar *)"/table/column");
    column_nodeset = columns->nodesetval;

    rowdata = 0; /* A flag since we will not want to commit an all(null) row */

    /* Iterate through columns and issue apprporiate bind SQL statements */
    for (i=0; i < column_nodeset->nodeNr; i++) {
        xmlChar *value;

        /* Obtain column value from xml */ 
        value = xmlNodeGetContent(column_nodeset->nodeTab[i]);

        /* Check if some non-null data exists for the row */
        if (rowdata==0 && xmlStrncmp(value, (xmlChar *)"", 1) != 0) {
            int j;

            /* Alter the flag */
            rowdata = 1; 

            /* Issue prepared stmt as some non-null data exists for the row */
            sqlite3_prepare_v2(db, (char *)sql_text, -1, &stmt, NULL);

            /* First column `id' is the record_number */
            sqlite3_bind_int(stmt, 1, record_number);

            /* Go back and bind previously skipped columns of nulls */
            for (j=0; j<i; j++)
                sqlite3_bind_null(stmt, j+2);
        }

        /* If rowData flag, bind data to prepared stmt as appropriate */
        if (rowdata == 1 && xmlStrncmp(value, (xmlChar *)"", 1) == 0)
            sqlite3_bind_null(stmt, i+2);
        else if (rowdata == 1 && xmlStrncmp(value, (xmlChar *)"", 1) != 0)
            sqlite3_bind_text(stmt, i+2, (char *)value, -1 , SQLITE_TRANSIENT);

        free(value);
    }

    if (rowdata == 1) {
        int rc;

        rc = sqlite3_step(stmt); 
        if (rc != SQLITE_DONE)
            fprintf(stderr, "ERROR inserting data: %s\n", sqlite3_errmsg(db));

        sqlite3_finalize(stmt);
    }

    xmlXPathFreeObject(columns);
    xmlFree(sql_text);
}


/**
 * normalizeXML:
 * @reader: an xmlReader 
 * 
 * Return a normalized record
 */
static xmlDocPtr
normalizeXML(xmlTextReaderPtr reader) {
    xmlChar *doc;
    xmlDocPtr result, xml;

    doc = xmlTextReaderReadOuterXml(reader);
    xml = xmlParseDoc(doc);
    xmlFree(doc);

    result = xsltApplyStylesheet(normalize_record, xml, NULL);
    xmlFreeDoc(xml);

    return result;
}


/**
 * writeSQL:
 * @norm_xml: a normalized xml document 
 * @record_number: integer for id in database 
 * 
 * Write xml record to database
 */
static void
writeSQL (xmlDocPtr norm_xml, int record_number){
    int i;
    xmlNodeSetPtr table_nodeset;
    xmlXPathObjectPtr tables;

    tables = getXPath(norm_xml, (xmlChar *)"/tables/table");
    table_nodeset = tables->nodesetval;

    for (i=0; i < table_nodeset->nodeNr; i++) {
        xmlChar *raw_table_xml;
        xmlBufferPtr buffer;
        xmlDocPtr parsed_table_xml;

        /* For each table, dump XML into a buffer and then parse
         * ?? Better way to get from xmlNodePtr --> xmlDocPtr ??
         */
        buffer = xmlBufferCreate();
        xmlNodeDump(buffer, norm_xml, table_nodeset->nodeTab[i], 2, 1);
        raw_table_xml = buffer-> content;
        xmlFree(buffer);

        parsed_table_xml = xmlParseDoc(raw_table_xml);
        xmlFree(raw_table_xml);

        /* Create the table in the database*/
        if (record_number == 1)
            buildTable(parsed_table_xml);

        /* Insert the record into the database */
        insertRecord(parsed_table_xml, record_number);

        xmlFreeDoc(parsed_table_xml);
    }

    xmlXPathFreeObject(tables);

}


/**
 * streamFile:
 * @filename: name of the xml file to parse
 * @record_number: starting integeger for id in database
 * 
 * Parse and process xml
 */
static void
streamFile(const char *filename, int record_number) {
    xmlTextReaderPtr reader;

    /* Open file in xmlReader */
    reader = xmlReaderForFile(filename, "UTF-8", 0);

    if (reader != NULL) {

        int ret = xmlTextReaderRead(reader);

        while (ret == 1) {
            int is_award_node, is_idv_node, node_type;

            is_award_node = xmlStrncmp(xmlTextReaderConstName(reader),
                                        (xmlChar *)"ns1:award", 10);
            is_idv_node = xmlStrncmp(xmlTextReaderConstName(reader),
                                        (xmlChar *)"ns1:IDV",8);
            node_type = xmlTextReaderNodeType(reader);
            if ((is_award_node == 0 || is_idv_node == 0) && node_type == 1) {
                xmlDocPtr norm_xml;
                
                record_number++;
                norm_xml = normalizeXML(reader);
                writeSQL(norm_xml, record_number);
                xmlFreeDoc(norm_xml);

            }

            ret = xmlTextReaderRead(reader);
        }

        xmlFreeTextReader(reader);

        if (ret != 0)
            fprintf(stderr, "Failed to parse %s\n", filename);

    } else
        fprintf(stderr, "Failed to open %s\n", filename);
}


int
main(int argc, char **argv) {
    char *err_msg = 0;
    int rc, record_number = 0;
    xmlDocPtr create_table_xsl_doc, insert_row_xsl_doc,
              normalize_record_xsl_doc;

    /* Check for ABI mismatches between compiled and shared libraries */
    LIBXML_TEST_VERSION

    /* Parse stylesheets and create transformations */
    create_table_xsl_doc = xmlReadMemory((const char *)create_table_xsl,
                        create_table_xsl_len, "null.xml", "UTF-8", 0);
    create_table = xsltParseStylesheetDoc(create_table_xsl_doc);
    
    insert_row_xsl_doc = xmlReadMemory((const char *)insert_row_xsl, 
                        insert_row_xsl_len, "null.xml", "UTF-8", 0);
    insert_row = xsltParseStylesheetDoc(insert_row_xsl_doc);
    
    normalize_record_xsl_doc = xmlReadMemory((const char *)normalize_record_xsl,
                        normalize_record_xsl_len, "null.xml", "UTF-8", 0);
    normalize_record = xsltParseStylesheetDoc(normalize_record_xsl_doc);

    /* Parse command line args */
    while (1) {
        static struct option long_options[] = {
            {"append",     no_argument,    NULL, 'a'},
            {"overwrite",  no_argument,    NULL, 'o'},
            {"help",       no_argument,    NULL, 'h'}
        };

        int option_index = 0;
        int c = getopt_long (argc, argv, "aoh", long_options, &option_index);
        if (c == -1)
            break;

        switch (c) {
        
            case 'a':
                aflg = 1;
                break;

            case 'o':
                oflg = 1;
                break;
            
            case 'h':
                hflg = 1;
                break;

            default:
                usage();
                cleanup();
                exit(EX_USAGE);
                break;
        }
    }
    
    /* Perform some checking on command line arguments
     */ 
    if (aflg && oflg) {
        /* Append and overwrite are mutually exclusive */
        fprintf(stderr, "-[a]ppend and -[o]verwrite cannot be used together\n");
        usage();
        cleanup();
        exit(EX_USAGE);
    } else if (hflg) {
        /* If --[h]elp is requested */
        usage();
        cleanup();
        exit(EX_USAGE);
    } else if ((argc-optind) != 2) {
        /* We must have exactly two remaining args: input and output files */
        usage();
        cleanup();
        exit(EX_USAGE);
    }

    /* Perform some checking on our input file
     */
    xml_archive = argv[optind++];
    if(access(xml_archive, F_OK ) != 0 || access(xml_archive, R_OK ) != 0) {
        /* If file does not exist or exists but is not readable */
        fprintf(stderr, "Error: input file cannot be read\n");
        cleanup();
        exit(EX_NOINPUT);
    }

    /* Perform some checking on our target file and truncate if requested
     */
    sqlite3_target = argv[optind++];
    if(access(sqlite3_target, F_OK) == 0 && !aflg && !oflg) {
        /* If file exists but no option is selected to append or overwrite */
        fprintf(stderr, "Error: output file exists, use -a or -o\n");
        cleanup();
        exit(EX_CANTCREAT);
    } else if(access(sqlite3_target, F_OK) == 0 &&
              access(sqlite3_target, W_OK) != 0) {
        /* If file exists but is not writable */
        fprintf(stderr, "Error: unable to open the output file for writing\n");
        cleanup();
        exit(EX_IOERR);
    } else if(access(sqlite3_target, F_OK) == 0 &&
              access(sqlite3_target, W_OK) == 0 && 
              oflg && truncate(sqlite3_target, 0) != 0) {
        /* If recv'd overwrite flag & file exists but os can't truncate */
        fprintf(stderr, "Failed to overwrite output file.\n");
        cleanup();
        exit(EX_IOERR);
    }
    
    /* Set up a sqlite3 connection to the target file / database
     */ 
    rc = sqlite3_open(sqlite3_target, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to open database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        cleanup();
        exit(EX_IOERR);
    }
    
    /* Obtain exclusive transaction with sqlite3
     */
    rc = sqlite3_exec(db, "BEGIN EXCLUSIVE TRANSACTION", NULL, NULL, &err_msg);
    
    /* If append flag then grab max record number from existing database
     */
    if (aflg) {
        char *stmt = "SELECT MAX(id) FROM awardcontractid;";
        sqlite3_stmt *res; 
        
        rc = sqlite3_prepare_v2(db, stmt, -1, &res, NULL);    
    
        if (rc != SQLITE_OK) {
            fprintf(stderr, "Failed to fetch data: %s\n", sqlite3_errmsg(db));
            sqlite3_free(err_msg);
            sqlite3_close(db);
            cleanup();
            exit(EX_DATAERR);
        } else if (sqlite3_step(res) != SQLITE_ROW) {
            fprintf(stderr, "Step failure: %s", sqlite3_errmsg(db));
            sqlite3_free(err_msg);
            sqlite3_close(db);
            cleanup();
            exit(EX_DATAERR);
        } else {
            record_number = sqlite3_column_int(res, 0);
            sqlite3_finalize(res);
        }
    }
    
    streamFile(xml_archive, record_number);
    createViews();

    sqlite3_exec(db, "END TRANSACTION", NULL, NULL, &err_msg);
    
    sqlite3_free(err_msg);
    sqlite3_close(db);
    cleanup();
    exit(0);

}