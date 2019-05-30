/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 *    Copyright 2019 (c) Matthias Konnerth
 */

#define _POSIX_C_SOURCE 199309L
#include "nodesetLoader.h"
#include "nodeset.h"
#include "sort.h"
#include "util.h"
#include <stdio.h>
#include <string.h>

static Nodeset *nodeset = NULL;

static size_t hierachicalRefCount = 7;

static bool isHierachicalReference(const Reference *ref) {
    for(size_t i = 0; i < hierachicalRefCount; i++) {
        if(!strcmp(ref->refType.idString, hierachicalReferences[i])) {
            return true;
        }
    }
    return false;
}

static void (*nodeCallback)(const TNode *) = NULL;

static char *getAttributeValue(NodeAttribute *attr, const char **attributes,
                               int nb_attributes) {
    const int fields = 5;
    for(int i = 0; i < nb_attributes; i++) {
        const char *localname = attributes[i * fields + 0];
        if(strcmp((char *)localname, attr->name))
            continue;
        const char *value_start = attributes[i * fields + 3];
        const char *value_end = attributes[i * fields + 4];
        size_t size = value_end - value_start;
        char *value = (char *)malloc(sizeof(char) * size + 1);
        nodeset->countedChars[nodeset->charsSize++] = value;
        memcpy(value, value_start, size);
        value[size] = '\0';
        return value;
    }
    if(attr->defaultValue != NULL || attr->optional) {
        return (char *)attr->defaultValue;
    }
    // todo: remove this assertation

    printf("attribute: %s\n", attr->name);
    assertf(false, "attribute not found, no default value set in getAttributeValue\n");
}

TNodeId alias2Id(const char *alias) {
    for(size_t cnt = 0; cnt < nodeset->aliasSize; cnt++) {
        if(strEqual(alias, nodeset->aliasArray[cnt]->name)) {
            return nodeset->aliasArray[cnt]->id;
        }
    }
    TNodeId id;
    id.id = 0;
    return id;
}

TNodeId translateNodeId(const TNamespace *namespaces, TNodeId id) {
    if(id.nsIdx > 0) {
        id.nsIdx = namespaces[id.nsIdx].idx;
        return id;
    }
    return id;
}

TNodeId extractNodedId(const TNamespace *namespaces, char *s) {
    if(s == NULL) {
        TNodeId id;
        id.id = 0;
        id.nsIdx = 0;
        id.idString = "null";
        return id;
    }
    TNodeId id;
    id.nsIdx = 0;
    id.idString = s;
    char *idxSemi = strchr(s, ';');
    if(idxSemi == NULL) {
        id.id = s;
        return id;
    } else {
        id.nsIdx = atoi(&s[3]);
        id.id = idxSemi + 1;
    }
    return translateNodeId(namespaces, id);
}

static void extractAttributes(const TNamespace *namespaces, TNode *node,
                              int attributeSize, const char **attributes) {
    node->id = extractNodedId(namespaces,
                              getAttributeValue(&attrNodeId, attributes, attributeSize));
    node->browseName = getAttributeValue(&attrBrowseName, attributes, attributeSize);
    switch(node->nodeClass) {
        case NODECLASS_OBJECTTYPE: {
            ((TObjectTypeNode *)node)->isAbstract =
                getAttributeValue(&attrIsAbstract, attributes, attributeSize);
            break;
        }
        case NODECLASS_OBJECT: {
            ((TObjectNode *)node)->parentNodeId =
                extractNodedId(namespaces, getAttributeValue(&attrParentNodeId,
                                                             attributes, attributeSize));
            ((TObjectNode *)node)->eventNotifier =
                getAttributeValue(&attrEventNotifier, attributes, attributeSize);
            break;
        }
        case NODECLASS_VARIABLE:
            ((TVariableNode *)node)->parentNodeId =
                extractNodedId(namespaces, getAttributeValue(&attrParentNodeId,
                                                             attributes, attributeSize));
            char *datatype = getAttributeValue(&attrDataType, attributes, attributeSize);
            TNodeId aliasId = alias2Id(datatype);
            if(aliasId.id != 0) {
                ((TVariableNode *)node)->datatype = aliasId;
            } else {
                ((TVariableNode *)node)->datatype = extractNodedId(namespaces, datatype);
            }
            ((TVariableNode *)node)->valueRank =
                getAttributeValue(&attrValueRank, attributes, attributeSize);
            ((TVariableNode *)node)->arrayDimensions =
                getAttributeValue(&attrArrayDimensions, attributes, attributeSize);

            break;
        case NODECLASS_DATATYPE:;
            break;
        case NODECLASS_METHOD:
            ((TMethodNode *)node)->parentNodeId =
                extractNodedId(namespaces, getAttributeValue(&attrParentNodeId,
                                                             attributes, attributeSize));
            break;
        case NODECLASS_REFERENCETYPE:;
            break;
        default:;
    }
}

static void initNode(TNamespace *namespaces, TNodeClass nodeClass, TNode *node,
                     int nb_attributes, const char **attributes) {
    node->nodeClass = nodeClass;
    node->hierachicalRefs = NULL;
    node->nonHierachicalRefs = NULL;
    extractAttributes(namespaces, node, nb_attributes, attributes);
}

static void extractReferenceAttributes(TParserCtx *ctx, int attributeSize,
                                       const char **attributes) {
    Reference *newRef = (Reference *)malloc(sizeof(Reference));
    nodeset->countedRefs[nodeset->refsSize++] = newRef;
    newRef->next = NULL;
    if(strEqual("true", getAttributeValue(&attrIsForward, attributes, attributeSize))) {
        newRef->isForward = true;
    } else {
        newRef->isForward = false;
    }
    newRef->refType =
        extractNodedId(nodeset->namespaceTable->namespace,
                       getAttributeValue(&attrReferenceType, attributes, attributeSize));
    if(isHierachicalReference(newRef)) {
        Reference **lastRef = &ctx->node->hierachicalRefs;
        while(*lastRef) {
            lastRef = &(*lastRef)->next;
        }
        *lastRef = newRef;
    } else {
        Reference **lastRef = &ctx->node->nonHierachicalRefs;
        while(*lastRef) {
            lastRef = &(*lastRef)->next;
        }
        *lastRef = newRef;
    }
    ctx->nextOnCharacters = &newRef->target.idString;
}

static void OnStartElementNs(void *ctx, const char *localname, const char *prefix,
                             const char *URI, int nb_namespaces, const char **namespaces,
                             int nb_attributes, int nb_defaulted,
                             const char **attributes) {

    TParserCtx *pctx = (TParserCtx *)ctx;
    switch(pctx->state) {
        case PARSER_STATE_INIT:
            if(strEqual(localname, ALIAS)) {
                pctx->state = PARSER_STATE_ALIAS;
                pctx->node = NULL;
                nodeset->aliasArray[nodeset->aliasSize] = malloc(sizeof(Alias));
                nodeset->aliasArray[nodeset->aliasSize]->name =
                    getAttributeValue(&attrAlias, attributes, nb_attributes);
                pctx->nextOnCharacters =
                    &nodeset->aliasArray[nodeset->aliasSize]->id.idString;
                pctx->state = PARSER_STATE_ALIAS;
            } else if(strEqual(localname, OBJECT)) {
                pctx->state = PARSER_STATE_NODE;
                pctx->nodeClass = NODECLASS_OBJECT;
                pctx->node = (TNode *)malloc(sizeof(TObjectNode));
                initNode(nodeset->namespaceTable->namespace, pctx->nodeClass, pctx->node,
                         nb_attributes, attributes);
                pctx->state = PARSER_STATE_NODE;
            } else if(strEqual(localname, OBJECTTYPE)) {
                pctx->state = PARSER_STATE_NODE;
                pctx->nodeClass = NODECLASS_OBJECTTYPE;
                pctx->node = (TNode *)malloc(sizeof(TObjectTypeNode));
                initNode(nodeset->namespaceTable->namespace, pctx->nodeClass, pctx->node,
                         nb_attributes, attributes);
                pctx->state = PARSER_STATE_NODE;
            } else if(strEqual(localname, VARIABLE)) {
                pctx->state = PARSER_STATE_NODE;
                pctx->nodeClass = NODECLASS_VARIABLE;
                pctx->node = (TNode *)malloc(sizeof(TVariableNode));
                initNode(nodeset->namespaceTable->namespace, pctx->nodeClass, pctx->node,
                         nb_attributes, attributes);
                pctx->state = PARSER_STATE_NODE;
            } else if(strEqual(localname, DATATYPE)) {
                pctx->state = PARSER_STATE_NODE;
                pctx->nodeClass = NODECLASS_DATATYPE;
                pctx->node = (TNode *)malloc(sizeof(TDataTypeNode));
                initNode(nodeset->namespaceTable->namespace, pctx->nodeClass, pctx->node,
                         nb_attributes, attributes);
                pctx->state = PARSER_STATE_NODE;
            } else if(strEqual(localname, METHOD)) {
                pctx->state = PARSER_STATE_NODE;
                pctx->nodeClass = NODECLASS_METHOD;
                pctx->node = (TNode *)malloc(sizeof(TMethodNode));
                initNode(nodeset->namespaceTable->namespace, pctx->nodeClass, pctx->node,
                         nb_attributes, attributes);
                pctx->state = PARSER_STATE_NODE;
            } else if(strEqual(localname, REFERENCETYPE)) {
                pctx->state = PARSER_STATE_NODE;
                pctx->nodeClass = NODECLASS_REFERENCETYPE;
                pctx->node = (TNode *)malloc(sizeof(TReferenceTypeNode));
                initNode(nodeset->namespaceTable->namespace, pctx->nodeClass, pctx->node,
                         nb_attributes, attributes);
                pctx->state = PARSER_STATE_NODE;
            } else if(strEqual(localname, NAMESPACEURIS)) {
                pctx->state = PARSER_STATE_NAMESPACEURIS;
            }
            break;
        case PARSER_STATE_NAMESPACEURIS:
            if(strEqual(localname, NAMESPACEURI)) {
                nodeset->namespaceTable->size++;
                TNamespace *namespaces = (TNamespace *)realloc(
                    nodeset->namespaceTable->namespace,
                    sizeof(TNamespace) * (nodeset->namespaceTable->size));
                nodeset->namespaceTable->namespace = namespaces;
                pctx->nextOnCharacters =
                    &namespaces[nodeset->namespaceTable->size - 1].name;
                pctx->state = PARSER_STATE_URI;
            }
            break;
        case PARSER_STATE_URI:
            break;
        case PARSER_STATE_NODE:
            if(strEqual(localname, DISPLAYNAME)) {
                pctx->nextOnCharacters = &pctx->node->displayName;
                pctx->state = PARSER_STATE_DISPLAYNAME;
                break;
            }
            if(strEqual(localname, REFERENCES)) {
                pctx->state = PARSER_STATE_REFERENCES;
                break;
            }
            if(strEqual(localname, DESCRIPTION)) {
                pctx->state = PARSER_STATE_DESCRIPTION;
                break;
            }
        case PARSER_STATE_REFERENCES:
            if(strEqual(localname, REFERENCE)) {
                pctx->state = PARSER_STATE_REFERENCE;
                extractReferenceAttributes(pctx, nb_attributes, attributes);
            }
            break;
        case PARSER_STATE_DESCRIPTION:
            break;
        case PARSER_STATE_ALIAS:
            break;
        case PARSER_STATE_DISPLAYNAME:
            break;
        case PARSER_STATE_REFERENCE:
            break;
        case PARSER_STATE_UNKNOWN:
            break;
    }
}

static void OnEndElementNs(void *ctx, const char *localname, const char *prefix,
                           const char *URI) {
    TParserCtx *pctx = (TParserCtx *)ctx;
    switch(pctx->state) {
        case PARSER_STATE_INIT:
            break;
        case PARSER_STATE_ALIAS: {
            nodeset->aliasArray[nodeset->aliasSize]->id =
                extractNodedId(nodeset->namespaceTable->namespace,
                               nodeset->aliasArray[nodeset->aliasSize]->id.idString);
            pctx->state = PARSER_STATE_INIT;
            nodeset->aliasSize++;
        } break;
        case PARSER_STATE_URI: {
            int globalIdx = nodeset->namespaceTable->cb(
                nodeset->namespaceTable->namespace[nodeset->namespaceTable->size - 1]
                    .name);

            nodeset->namespaceTable->namespace[nodeset->namespaceTable->size - 1].idx =
                globalIdx;
            pctx->state = PARSER_STATE_NAMESPACEURIS;
        } break;
        case PARSER_STATE_NAMESPACEURIS:
            pctx->state = PARSER_STATE_INIT;
            break;
        case PARSER_STATE_NODE:
            if(strEqual(localname, OBJECT) || strEqual(localname, VARIABLE) ||
               strEqual(localname, OBJECTTYPE) || strEqual(localname, DATATYPE) ||
               strEqual(localname, METHOD)) {
                nodeCallback(pctx->node);
                pctx->state = PARSER_STATE_INIT;
            }
            if(strEqual(localname, REFERENCETYPE)) {
                Reference *ref = pctx->node->hierachicalRefs;
                while(ref) {
                    if(!ref->isForward) {
                        hierachicalReferences[hierachicalRefCount++] = pctx->node->id.id;
                        break;
                    }
                    ref = ref->next;
                }
                nodeCallback(pctx->node);
                pctx->state = PARSER_STATE_INIT;
            }
            break;
        case PARSER_STATE_DESCRIPTION:
        case PARSER_STATE_DISPLAYNAME:
        case PARSER_STATE_REFERENCES:
            if(strEqual(localname, DESCRIPTION) || strEqual(localname, DISPLAYNAME) ||
               strEqual(localname, REFERENCES)) {
                pctx->state = PARSER_STATE_NODE;
            }
            break;
        case PARSER_STATE_REFERENCE: {
            Reference *ref = pctx->node->hierachicalRefs;
            while(ref) {
                ref->target = extractNodedId(nodeset->namespaceTable->namespace,
                                             ref->target.idString);
                ref = ref->next;
            }
            ref = pctx->node->nonHierachicalRefs;
            while(ref) {
                ref->target = extractNodedId(nodeset->namespaceTable->namespace,
                                             ref->target.idString);
                ref = ref->next;
            }
            pctx->state = PARSER_STATE_REFERENCES;
        } break;
        case PARSER_STATE_UNKNOWN:;
    }
}

static void OnCharacters(void *ctx, const char *ch, int len) {
    TParserCtx *pctx = (TParserCtx *)ctx;
    if(pctx->nextOnCharacters == NULL)
        return;
    char *value = malloc(len + 1);
    nodeset->countedChars[nodeset->charsSize++] = value;
    strncpy(value, ch, len);
    value[len] = '\0';
    pctx->nextOnCharacters[0] = value;
    pctx->nextOnCharacters = NULL;
}

static xmlSAXHandler make_sax_handler() {
    xmlSAXHandler SAXHandler;
    memset(&SAXHandler, 0, sizeof(xmlSAXHandler));
    SAXHandler.initialized = XML_SAX2_MAGIC;
    SAXHandler.startElementNs = OnStartElementNs;
    SAXHandler.endElementNs = OnEndElementNs;
    SAXHandler.characters = OnCharacters;
    return SAXHandler;
}

int read_xmlfile(FILE *f, TParserCtx *parserCtxt) {
    char chars[1024];
    int res = fread(chars, 1, 4, f);
    if(res <= 0) {
        return 1;
    }

    xmlSAXHandler SAXHander = make_sax_handler();
    xmlParserCtxtPtr ctxt =
        xmlCreatePushParserCtxt(&SAXHander, parserCtxt, chars, res, NULL);
    while((res = fread(chars, 1, sizeof(chars), f)) > 0) {
        if(xmlParseChunk(ctxt, chars, res, 0)) {
            xmlParserError(ctxt, "xmlParseChunk");
            return 1;
        }
    }
    xmlParseChunk(ctxt, chars, 0, 1);
    xmlFreeParserCtxt(ctxt);
    xmlCleanupParser();
    return 0;
}

void addNodeInternal(const TNode *node) {
    size_t cnt = nodeset->nodes[node->nodeClass]->cnt;
    nodeset->nodes[node->nodeClass]->nodes[cnt] = node;
    nodeset->nodes[node->nodeClass]->cnt++;
}

void initCounters() { hierachicalRefCount = 7; }

static void freeMemory(TParserCtx *ctx) {

    Nodeset *n = nodeset;
    // free chars
    for(size_t cnt = 0; cnt < n->charsSize; cnt++) {
        free((void *)n->countedChars[cnt]);
    }
    free(n->countedChars);

    // free refs
    for(size_t cnt = 0; cnt < n->refsSize; cnt++) {
        free((void *)n->countedRefs[cnt]);
    }
    free(n->countedRefs);

    // free alias
    for(size_t cnt = 0; cnt < n->aliasSize; cnt++) {
        free(n->aliasArray[cnt]);
    }
    free(n->aliasArray);

    for(size_t cnt = 0; cnt < 6; cnt++) {
        size_t storedNodes = n->nodes[cnt]->cnt;
        for(size_t nodeCnt = 0; nodeCnt < storedNodes; nodeCnt++) {
            free((void *)n->nodes[cnt]->nodes[nodeCnt]);
        }
        free((void *)n->nodes[cnt]->nodes);
        free((void *)n->nodes[cnt]);
    }

    // free namespacetable, nodeset
    free(n->namespaceTable->namespace);
    free(n->namespaceTable);
    free(n);

    free(ctx);
}

static void setupNodeset() {
    nodeset = malloc(sizeof(Nodeset));
    nodeset->aliasArray = malloc(sizeof(Alias *) * MAX_ALIAS);
    nodeset->aliasSize = 0;
    nodeset->countedRefs = malloc(sizeof(Reference *) * MAX_REFCOUNTEDREFS);
    nodeset->refsSize = 0;
    nodeset->countedChars = malloc(sizeof(char *) * MAX_REFCOUNTEDCHARS);
    nodeset->charsSize = 0;
    nodeset->nodes[NODECLASS_OBJECT] = malloc(sizeof(NodeContainer));
    nodeset->nodes[NODECLASS_OBJECT]->nodes = malloc(sizeof(TNode *) * MAX_OBJECTS);
    nodeset->nodes[NODECLASS_OBJECT]->cnt = 0;
    nodeset->nodes[NODECLASS_VARIABLE] = malloc(sizeof(NodeContainer));
    nodeset->nodes[NODECLASS_VARIABLE]->nodes = malloc(sizeof(TNode *) * MAX_VARIABLES);
    nodeset->nodes[NODECLASS_VARIABLE]->cnt = 0;
    nodeset->nodes[NODECLASS_METHOD] = malloc(sizeof(NodeContainer));
    nodeset->nodes[NODECLASS_METHOD]->nodes = malloc(sizeof(TNode *) * MAX_METHODS);
    nodeset->nodes[NODECLASS_METHOD]->cnt = 0;
    nodeset->nodes[NODECLASS_OBJECTTYPE] = malloc(sizeof(NodeContainer));
    nodeset->nodes[NODECLASS_OBJECTTYPE]->nodes = malloc(sizeof(TNode *) * MAX_DATATYPES);
    nodeset->nodes[NODECLASS_OBJECTTYPE]->cnt = 0;
    nodeset->nodes[NODECLASS_DATATYPE] = malloc(sizeof(NodeContainer));
    nodeset->nodes[NODECLASS_DATATYPE]->nodes =
        malloc(sizeof(TNode *) * MAX_REFERENCETYPES);
    nodeset->nodes[NODECLASS_DATATYPE]->cnt = 0;
    nodeset->nodes[NODECLASS_REFERENCETYPE] = malloc(sizeof(NodeContainer));
    nodeset->nodes[NODECLASS_REFERENCETYPE]->nodes =
        malloc(sizeof(TNode *) * MAX_REFERENCETYPES);
    nodeset->nodes[NODECLASS_REFERENCETYPE]->cnt = 0;
}

void loadFile(const FileHandler *fileHandler) {
    struct timespec start, startSort, startAdd, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    setupNodeset();
    initCounters();
    FILE *f = fopen(fileHandler->file, "r");
    nodeCallback = insertNode;
    init();

    TParserCtx *ctx = (TParserCtx *)malloc(sizeof(TParserCtx));
    ctx->state = PARSER_STATE_INIT;
    ctx->nextOnCharacters = NULL;

    TNamespaceTable *table = malloc(sizeof(TNamespaceTable));
    table->cb = fileHandler->addNamespace;
    table->size = 1;
    table->namespace = malloc(sizeof(TNamespace));
    table->namespace[0].idx = 0;
    table->namespace[0].name = "opcfoundation";
    nodeset->namespaceTable = table;

    if(!f) {
        puts("file open error.");
        exit(1);
    }

    if(read_xmlfile(f, ctx)) {
        puts("xml read error.");
        exit(1);
    }

    clock_gettime(CLOCK_MONOTONIC, &startSort);
    sort(addNodeInternal);
    clock_gettime(CLOCK_MONOTONIC, &startAdd);

    for(size_t cnt = 0; cnt < nodeset->nodes[NODECLASS_REFERENCETYPE]->cnt; cnt++) {
        fileHandler->callback(nodeset->nodes[NODECLASS_REFERENCETYPE]->nodes[cnt]);
    }

    for(size_t cnt = 0; cnt < nodeset->nodes[NODECLASS_OBJECTTYPE]->cnt; cnt++) {
        fileHandler->callback(nodeset->nodes[NODECLASS_OBJECTTYPE]->nodes[cnt]);
    }

    for(size_t cnt = 0; cnt < nodeset->nodes[NODECLASS_OBJECT]->cnt; cnt++) {
        fileHandler->callback(nodeset->nodes[NODECLASS_OBJECT]->nodes[cnt]);
    }

    for(size_t cnt = 0; cnt < nodeset->nodes[NODECLASS_METHOD]->cnt; cnt++) {
        fileHandler->callback(nodeset->nodes[NODECLASS_METHOD]->nodes[cnt]);
    }

    for(size_t cnt = 0; cnt < nodeset->nodes[NODECLASS_METHOD]->cnt; cnt++) {
        fileHandler->callback(nodeset->nodes[NODECLASS_METHOD]->nodes[cnt]);
    }

    for(size_t cnt = 0; cnt < nodeset->nodes[NODECLASS_VARIABLE]->cnt; cnt++) {
        fileHandler->callback(nodeset->nodes[NODECLASS_VARIABLE]->nodes[cnt]);
    }

    freeMemory(ctx);

    fclose(f);
    clock_gettime(CLOCK_MONOTONIC, &end);

    struct timespec parse = diff(start, startSort);
    struct timespec sort = diff(startSort, startAdd);
    struct timespec add = diff(startAdd, end);
    struct timespec sum = diff(start, end);
    printf("parse (s, ms): %lu %lu\n", parse.tv_sec, parse.tv_nsec / 1000000);
    printf("sort (s, ms): %lu %lu\n", sort.tv_sec, sort.tv_nsec / 1000000);
    printf("add (s, ms): %lu %lu\n", add.tv_sec, add.tv_nsec / 1000000);
    printf("sum (s, ms): %lu %lu\n", sum.tv_sec, sum.tv_nsec / 1000000);
}