/*
 *
 *  Copyright (C) 2026, OFFIS e.V.
 *  All rights reserved.  See COPYRIGHT file for details.
 *
 *  This software and supporting documentation were developed by
 *
 *    OFFIS e.V.
 *    R&D Division Health
 *    Escherweg 2
 *    D-26121 Oldenburg, Germany
 *
 *
 *  Module:  dcmseg
 *
 *  Author:  Michael Onken
 *
 *  Purpose: Tests for DcmSegmentation segment storage (OFVector + compat layer)
 *
 */

#include "dcmtk/config/osconfig.h"

#include "dcmtk/dcmdata/dcdict.h"
#include "dcmtk/dcmdata/dcdeftag.h"
#include "dcmtk/dcmiod/iodmacro.h"
#include "dcmtk/dcmseg/segdoc.h"
#include "dcmtk/dcmseg/segment.h"
#include "dcmtk/dcmseg/segtypes.h"
#include "dcmtk/ofstd/ofmap.h"
#include "dcmtk/ofstd/oftest.h"

static DcmSegment* makeSegment(const char* label)
{
    DcmSegment* seg = NULL;
    CodeSequenceMacro category("85756007", "SCT", "Tissue");
    CodeSequenceMacro propType("51114001", "SCT", "Artery");
    DcmSegment::create(seg, label, category, propType, DcmSegTypes::SAT_AUTOMATIC, "OC_DUMMY");
    return seg;
}

static DcmSegmentation* makeBinarySeg()
{
    IODGeneralEquipmentModule::EquipmentInfo eq("OC", "OC_PRODUCT", "OC_DEVICE", "0.1");
    ContentIdentificationMacro ci("1", "LABEL", "DESCRIPTION", "Doe^John");
    DcmSegmentation* seg = NULL;
    DcmSegmentation::createBinarySegmentation(seg, 2, 2, eq, ci);
    return seg;
}

static DcmSegmentation* makeLabelmapSeg()
{
    IODGeneralEquipmentModule::EquipmentInfo eq("OC", "OC_PRODUCT", "OC_DEVICE", "0.1");
    ContentIdentificationMacro ci("1", "LABEL", "DESCRIPTION", "Doe^John");
    DcmSegmentation* seg = NULL;
    DcmSegmentation::createLabelmapSegmentation(seg, 2, 2, eq, ci, OFFalse);
    return seg;
}


// Test basic addSegment / getSegment / getNumberOfSegments for binary segmentation
// (1-based segment numbers)
OFTEST(dcmseg_segments_add_get_binary)
{
    if (!dcmDataDict.isDictionaryLoaded())
    {
        OFCHECK_FAIL("no data dictionary loaded, check environment variable: " DCM_DICT_ENVIRONMENT_VARIABLE);
        return;
    }

    DcmSegmentation* seg = makeBinarySeg();
    OFCHECK(seg != NULL);
    if (!seg) return;

    OFCHECK(seg->getNumberOfSegments() == 0);

    // Add 5 segments; binary segmentations assign 1-based numbers automatically
    Uint16 segNum = 0;
    for (int i = 0; i < 5; i++)
    {
        char label[32];
        OFStandard::snprintf(label, sizeof(label), "Seg%d", i);
        DcmSegment* s = makeSegment(label);
        OFCHECK(s != NULL);
        OFCHECK(seg->addSegment(s, segNum).good());
        OFCHECK(segNum == OFstatic_cast(Uint16, i + 1));
    }
    OFCHECK(seg->getNumberOfSegments() == 5);

    // Retrieve each segment
    for (Uint16 i = 1; i <= 5; i++)
    {
        DcmSegment* s = seg->getSegment(i);
        OFCHECK(s != NULL);
    }

    // Segment 0 is invalid for binary
    OFCHECK(seg->getSegment(0) == NULL);

    // Out-of-range returns NULL
    OFCHECK(seg->getSegment(6) == NULL);
    OFCHECK(seg->getSegment(1000) == NULL);

    delete seg;
}


// Test addSegment / getSegment for labelmap (0-based segment numbers)
OFTEST(dcmseg_segments_add_get_labelmap)
{
    if (!dcmDataDict.isDictionaryLoaded())
    {
        OFCHECK_FAIL("no data dictionary loaded, check environment variable: " DCM_DICT_ENVIRONMENT_VARIABLE);
        return;
    }

    DcmSegmentation* seg = makeLabelmapSeg();
    OFCHECK(seg != NULL);
    if (!seg) return;

    OFCHECK(seg->getNumberOfSegments() == 0);

    // Add segments with explicit 0-based numbers
    for (Uint16 i = 0; i < 5; i++)
    {
        char label[32];
        OFStandard::snprintf(label, sizeof(label), "Label%u", i);
        DcmSegment* s = makeSegment(label);
        OFCHECK(s != NULL);
        Uint16 segNum = i;
        OFCHECK(seg->addSegment(s, segNum).good());
    }
    OFCHECK(seg->getNumberOfSegments() == 5);

    // Retrieve each - including segment 0
    for (Uint16 i = 0; i < 5; i++)
    {
        DcmSegment* s = seg->getSegment(i);
        OFCHECK(s != NULL);
    }

    // Out-of-range returns NULL
    OFCHECK(seg->getSegment(5) == NULL);

    delete seg;
}


// Test getSegmentNumber (reverse lookup: pointer -> number)
OFTEST(dcmseg_segments_get_segment_number)
{
    if (!dcmDataDict.isDictionaryLoaded())
    {
        OFCHECK_FAIL("no data dictionary loaded, check environment variable: " DCM_DICT_ENVIRONMENT_VARIABLE);
        return;
    }

    DcmSegmentation* seg = makeBinarySeg();
    OFCHECK(seg != NULL);
    if (!seg) return;

    DcmSegment* ptrs[3];
    for (int i = 0; i < 3; i++)
    {
        char label[32];
        OFStandard::snprintf(label, sizeof(label), "S%d", i);
        ptrs[i] = makeSegment(label);
        Uint16 num = 0;
        OFCHECK(seg->addSegment(ptrs[i], num).good());
    }

    // Reverse lookup each pointer
    for (int i = 0; i < 3; i++)
    {
        size_t foundNum = 999;
        OFCHECK(seg->getSegmentNumber(ptrs[i], foundNum) == OFTrue);
        OFCHECK(foundNum == OFstatic_cast(size_t, i + 1));
    }

    // NULL pointer returns OFFalse
    size_t dummy = 0;
    OFCHECK(seg->getSegmentNumber(NULL, dummy) == OFFalse);

    // Unknown pointer returns OFFalse
    DcmSegment* unknown = makeSegment("unknown");
    OFCHECK(seg->getSegmentNumber(unknown, dummy) == OFFalse);
    delete unknown;

    delete seg;
}


// Test the getSegments() compat layer:
// returns a correctly populated OFMap that reflects the internal vector state
OFTEST(dcmseg_segments_compat_layer)
{
    if (!dcmDataDict.isDictionaryLoaded())
    {
        OFCHECK_FAIL("no data dictionary loaded, check environment variable: " DCM_DICT_ENVIRONMENT_VARIABLE);
        return;
    }

    DcmSegmentation* seg = makeBinarySeg();
    OFCHECK(seg != NULL);
    if (!seg) return;

    // Empty state: compat map should be empty
    const OFMap<Uint16, DcmSegment*>& map0 = seg->getSegments();
    OFCHECK(map0.size() == 0);

    // Add 3 segments
    for (int i = 0; i < 3; i++)
    {
        char label[32];
        OFStandard::snprintf(label, sizeof(label), "C%d", i);
        Uint16 num = 0;
        OFCHECK(seg->addSegment(makeSegment(label), num).good());
    }

    // Compat map should reflect the 3 segments with keys 1, 2, 3
    const OFMap<Uint16, DcmSegment*>& map1 = seg->getSegments();
    OFCHECK(map1.size() == 3);

    // Verify keys and values match getSegment()
    OFMap<Uint16, DcmSegment*>::const_iterator it = map1.begin();
    Uint16 expectedKey = 1;
    while (it != map1.end())
    {
        OFCHECK(it->first == expectedKey);
        OFCHECK(it->second == seg->getSegment(expectedKey));
        expectedKey++;
        it++;
    }

    delete seg;
}


// Test the compat layer for labelmaps (0-based keys in the map)
OFTEST(dcmseg_segments_compat_layer_labelmap)
{
    if (!dcmDataDict.isDictionaryLoaded())
    {
        OFCHECK_FAIL("no data dictionary loaded, check environment variable: " DCM_DICT_ENVIRONMENT_VARIABLE);
        return;
    }

    DcmSegmentation* seg = makeLabelmapSeg();
    OFCHECK(seg != NULL);
    if (!seg) return;

    // Add segments 0..4
    for (Uint16 i = 0; i < 5; i++)
    {
        char label[32];
        OFStandard::snprintf(label, sizeof(label), "L%u", i);
        Uint16 num = i;
        OFCHECK(seg->addSegment(makeSegment(label), num).good());
    }

    const OFMap<Uint16, DcmSegment*>& map = seg->getSegments();
    OFCHECK(map.size() == 5);

    // Verify keys 0..4 are present
    for (Uint16 i = 0; i < 5; i++)
    {
        OFMap<Uint16, DcmSegment*>::const_iterator it = map.find(i);
        OFCHECK(it != map.end());
        if (it != map.end())
        {
            OFCHECK(it->second == seg->getSegment(i));
        }
    }

    delete seg;
}


// Test adding many segments (exercises vector resizing)
OFTEST(dcmseg_segments_many)
{
    if (!dcmDataDict.isDictionaryLoaded())
    {
        OFCHECK_FAIL("no data dictionary loaded, check environment variable: " DCM_DICT_ENVIRONMENT_VARIABLE);
        return;
    }

    DcmSegmentation* seg = makeBinarySeg();
    OFCHECK(seg != NULL);
    if (!seg) return;

    const Uint16 count = 500;
    for (Uint16 i = 0; i < count; i++)
    {
        char label[32];
        OFStandard::snprintf(label, sizeof(label), "M%u", i);
        Uint16 num = 0;
        OFCHECK(seg->addSegment(makeSegment(label), num).good());
        OFCHECK(num == i + 1);
    }
    OFCHECK(seg->getNumberOfSegments() == count);

    // Spot-check retrieval
    OFCHECK(seg->getSegment(1) != NULL);
    OFCHECK(seg->getSegment(250) != NULL);
    OFCHECK(seg->getSegment(500) != NULL);
    OFCHECK(seg->getSegment(501) == NULL);

    // Compat map should also have 500 entries
    const OFMap<Uint16, DcmSegment*>& map = seg->getSegments();
    OFCHECK(map.size() == count);

    delete seg;
}


// Test sparse labelmap segment numbers (non-contiguous)
OFTEST(dcmseg_segments_sparse_labelmap)
{
    if (!dcmDataDict.isDictionaryLoaded())
    {
        OFCHECK_FAIL("no data dictionary loaded, check environment variable: " DCM_DICT_ENVIRONMENT_VARIABLE);
        return;
    }

    DcmSegmentation* seg = makeLabelmapSeg();
    OFCHECK(seg != NULL);
    if (!seg) return;

    // Add segments at non-contiguous positions: 0, 5, 10, 50
    Uint16 positions[] = {0, 5, 10, 50};
    size_t numPositions = 4;
    for (size_t i = 0; i < numPositions; i++)
    {
        char label[32];
        OFStandard::snprintf(label, sizeof(label), "Sparse%u", positions[i]);
        Uint16 num = positions[i];
        OFCHECK(seg->addSegment(makeSegment(label), num).good());
    }

    OFCHECK(seg->getNumberOfSegments() == numPositions);

    // Populated slots should return non-NULL
    for (size_t i = 0; i < numPositions; i++)
    {
        OFCHECK(seg->getSegment(positions[i]) != NULL);
    }

    // Gaps should return NULL
    OFCHECK(seg->getSegment(1) == NULL);
    OFCHECK(seg->getSegment(7) == NULL);
    OFCHECK(seg->getSegment(20) == NULL);

    // Compat map should have exactly numPositions entries
    const OFMap<Uint16, DcmSegment*>& map = seg->getSegments();
    OFCHECK(map.size() == numPositions);

    // Verify only the correct keys are present
    for (size_t i = 0; i < numPositions; i++)
    {
        OFMap<Uint16, DcmSegment*>::const_iterator it = map.find(positions[i]);
        OFCHECK(it != map.end());
    }
    // A gap key should not be in the map
    OFCHECK(map.find(1) == map.end());

    delete seg;
}


// Test that calling getSegments() multiple times without mutation returns
// consistent results (compat map caching)
OFTEST(dcmseg_segments_compat_caching)
{
    if (!dcmDataDict.isDictionaryLoaded())
    {
        OFCHECK_FAIL("no data dictionary loaded, check environment variable: " DCM_DICT_ENVIRONMENT_VARIABLE);
        return;
    }

    DcmSegmentation* seg = makeBinarySeg();
    OFCHECK(seg != NULL);
    if (!seg) return;

    Uint16 num = 0;
    OFCHECK(seg->addSegment(makeSegment("A"), num).good());
    OFCHECK(seg->addSegment(makeSegment("B"), num).good());

    // First call builds the map
    const OFMap<Uint16, DcmSegment*>& map1 = seg->getSegments();
    OFCHECK(map1.size() == 2);

    // Second call without mutation should return same content
    const OFMap<Uint16, DcmSegment*>& map2 = seg->getSegments();
    OFCHECK(map2.size() == 2);

    // Add another segment - compat map should update
    OFCHECK(seg->addSegment(makeSegment("C"), num).good());
    const OFMap<Uint16, DcmSegment*>& map3 = seg->getSegments();
    OFCHECK(map3.size() == 3);

    delete seg;
}
