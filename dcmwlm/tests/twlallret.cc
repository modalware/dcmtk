/*
 *
 *  Copyright (C) 2026, Open Connections GmbH
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
 *  Module:  dcmwlm
 *
 *  Author:  Michael Onken
 *
 *  Purpose: Tests for accepting arbitrary (non-sequence) return key
 *           attributes in the wlmscpfs filesystem worklist data source
 *           (option --enable-all-return).
 *
 */

#include "dcmtk/config/osconfig.h"

#include "dcmtk/dcmdata/dcdatset.h"
#include "dcmtk/dcmdata/dcdeftag.h"
#include "dcmtk/dcmdata/dcfilefo.h"
#include "dcmtk/dcmdata/dcitem.h"
#include "dcmtk/dcmwlm/wldsfs.h"
#include "dcmtk/dcmwlm/wltypdef.h"
#include "dcmtk/ofstd/offile.h"
#include "dcmtk/ofstd/offilsys.h"
#include "dcmtk/ofstd/oflist.h"
#include "dcmtk/ofstd/ofstd.h"
#include "dcmtk/ofstd/ofstring.h"
#include "dcmtk/ofstd/oftempf.h"
#include "dcmtk/ofstd/oftest.h"

#ifdef _WIN32
#include <direct.h>
#endif
#ifdef HAVE_UNISTD_H
BEGIN_EXTERN_C
#include <unistd.h>
END_EXTERN_C
#endif

// AE title used as the worklist sub-directory; must be a valid filesystem AE title.
#define ALLRET_AETITLE "ALLRETWL"

// ----- Small filesystem helpers -------------------------------------------

static void removeDir(const OFString& path)
{
#ifdef _WIN32
    _rmdir(path.c_str());
#else
    rmdir(path.c_str());
#endif
}

// Create an empty file (used for the worklist lock file).
static OFBool touchFile(const OFString& path)
{
    OFFile f;
    if (!f.fopen(path.c_str(), "wb"))
        return OFFalse;
    f.fclose();
    return OFTrue;
}

// ----- Worklist record (.wl file) construction ----------------------------

// Build a single worklist file that carries, besides the standard patient
// identification, two attributes that are NOT part of the supported return
// key set of wlmscpfs: Patient's Telephone Numbers (0010,2154) and, as an
// arbitrary representative of the binary value representations, Rows
// (0028,0010). Write it to 'fileName' as a bare data set (no file meta
// information, like a real wlmscpfs .wl file).
static OFBool writeWorklistFile(const OFString& fileName)
{
    DcmFileFormat ff;
    DcmDataset* ds = ff.getDataset();

    ds->putAndInsertString(DCM_PatientName, "Mustermann^Max");
    ds->putAndInsertString(DCM_PatientID, "PID4711");
    ds->putAndInsertString(DCM_OtherPatientNames, "Alias^One\\Alias^Two");
    ds->putAndInsertString(DCM_PatientTelephoneNumbers, "0123-456789");
    // arbitrary attribute with a binary value representation (US)
    ds->putAndInsertUint16(DCM_Rows, 512);

    return ff
        .saveFile(fileName.c_str(),
                  EXS_LittleEndianExplicit,
                  EET_ExplicitLength,
                  EGL_recalcGL,
                  EPD_withoutPadding,
                  0,
                  0,
                  EWM_dataset)
        .good();
}

// ----- Query execution helpers --------------------------------------------

// Run a single C-FIND against the data source and collect all responses.
// 'sawWarning' is set to OFTrue if any response carried the "pending with
// warning" status that flags unsupported optional keys in the search mask.
// The caller owns the returned data sets (see freeResults()).
static size_t runQuery(WlmDataSourceFileSystem& wdb, DcmDataset& mask,
                       OFList<DcmDataset*>& results, OFBool& sawWarning)
{
    sawWarning = OFFalse;
    WlmDataSourceStatusType status = wdb.StartFindRequest(mask);
    while (status == WLM_PENDING || status == WLM_PENDING_WARNING)
    {
        if (status == WLM_PENDING_WARNING)
            sawWarning = OFTrue;
        DcmDataset* rsp = wdb.NextFindResponse(status);
        if (rsp == NULL)
            break;
        results.push_back(rsp);
    }
    return results.size();
}

static void freeResults(OFList<DcmDataset*>& results)
{
    for (OFListIterator(DcmDataset*) it = results.begin(); it != results.end(); ++it)
        delete *it;
    results.clear();
}

OFTEST(dcmwlm_all_return_keys)
{
    // ----- Set up a temporary worklist database on disk. -----
    OFString tempBase;
    OFTempFile::getTempPath(tempBase);

    char suffix[64];
    OFStandard::snprintf(suffix, sizeof(suffix), "dcmwlm_allret_%ld", OFStandard::getProcessID());

    const OFString rootDir  = (OFpath(tempBase) / suffix).native();
    const OFString aeDir    = (OFpath(rootDir) / ALLRET_AETITLE).native();
    const OFString lockFile = (OFpath(aeDir) / "lockfile").native();
    const OFString wlFile   = (OFpath(aeDir) / "allret01.wl").native();

    OFCHECK(OFStandard::createDirectory(rootDir, tempBase).good());
    OFCHECK(OFStandard::createDirectory(aeDir, rootDir).good());
    OFCHECK(touchFile(lockFile));
    OFCHECK(writeWorklistFile(wlFile));

    // ----- Connect the filesystem data source to that database. -----
    WlmDataSourceFileSystem wdb;
    wdb.SetDfPath(rootDir);
    wdb.SetCalledApplicationEntityTitle(ALLRET_AETITLE);
    // keep the worklist file minimal: do not require all type 1 return keys
    wdb.SetEnableRejectionOfIncompleteWlFiles(OFFalse);
    OFCHECK(wdb.ConnectToDataSource().good());
    OFCHECK(wdb.IsCalledApplicationEntityTitleSupported());

    // ----- Test 1: default behavior, unsupported return key is dropped. -----
    // With the option disabled (the default), an unsupported non-sequence
    // return key must be removed from the response and flagged with the
    // "pending with warning" status. Also checks the default value handling
    // of the supported return key Pregnancy Status.
    {
        DcmDataset mask;
        mask.insertEmptyElement(DCM_PatientName);
        mask.insertEmptyElement(DCM_PatientTelephoneNumbers);
        mask.insertEmptyElement(DCM_PregnancyStatus);

        OFList<DcmDataset*> results;
        OFBool sawWarning = OFFalse;
        OFCHECK(runQuery(wdb, mask, results, sawWarning) == 1);
        OFCHECK(sawWarning);
        if (!results.empty())
        {
            OFCHECK(!results.front()->tagExists(DCM_PatientTelephoneNumbers));
            // Pregnancy Status is a supported return key; if it is absent from
            // the worklist file, it must be returned with value 4 ("unknown").
            Uint16 pregnancyStatus = 0;
            OFCHECK(results.front()->findAndGetUint16(DCM_PregnancyStatus, pregnancyStatus).good());
            OFCHECK_EQUAL(pregnancyStatus, 4);
        }
        freeResults(results);
    }

    // ----- Test 2: option enabled, unsupported return key is filled. -----
    // With the option enabled, unsupported non-sequence return keys must be
    // returned with the values from the worklist file, without the warning
    // status. This also covers attributes with a binary value representation
    // (here: Rows, US). Supported return keys (here: Other Patient Names)
    // must be returned as before.
    wdb.SetEnableAllReturnKeys(OFTrue);
    {
        DcmDataset mask;
        mask.insertEmptyElement(DCM_PatientName);
        mask.insertEmptyElement(DCM_OtherPatientNames);
        mask.insertEmptyElement(DCM_PatientTelephoneNumbers);
        mask.insertEmptyElement(DCM_Rows);

        OFList<DcmDataset*> results;
        OFBool sawWarning = OFFalse;
        OFCHECK(runQuery(wdb, mask, results, sawWarning) == 1);
        OFCHECK(!sawWarning);
        if (!results.empty())
        {
            OFString value;
            OFCHECK(results.front()->findAndGetOFString(DCM_PatientTelephoneNumbers, value).good());
            OFCHECK_EQUAL(value, OFString("0123-456789"));
            OFCHECK(results.front()->findAndGetOFStringArray(DCM_OtherPatientNames, value).good());
            OFCHECK_EQUAL(value, OFString("Alias^One\\Alias^Two"));
            Uint16 rows = 0;
            OFCHECK(results.front()->findAndGetUint16(DCM_Rows, rows).good());
            OFCHECK_EQUAL(rows, 512);
        }
        freeResults(results);
    }

    // ----- Test 3: option enabled, attribute absent from worklist file. -----
    // An accepted return key that is not present in the worklist file must be
    // returned with an empty value, again without the warning status.
    {
        DcmDataset mask;
        mask.insertEmptyElement(DCM_PatientName);
        mask.insertEmptyElement(DCM_PatientMotherBirthName);

        OFList<DcmDataset*> results;
        OFBool sawWarning = OFFalse;
        OFCHECK(runQuery(wdb, mask, results, sawWarning) == 1);
        OFCHECK(!sawWarning);
        if (!results.empty())
        {
            OFCHECK(results.front()->tagExists(DCM_PatientMotherBirthName));
            OFString value;
            results.front()->findAndGetOFString(DCM_PatientMotherBirthName, value);
            OFCHECK_EQUAL(value, OFString(""));
        }
        freeResults(results);
    }

    // ----- Test 4: option enabled, unsupported sequence is still dropped. -----
    // The option only covers non-sequence attributes on the main level;
    // an unsupported sequence must still be removed and flagged.
    {
        DcmDataset mask;
        mask.insertEmptyElement(DCM_PatientName);
        DcmItem* item = NULL;
        mask.findOrCreateSequenceItem(DCM_ProcedureCodeSequence, item, 0);

        OFList<DcmDataset*> results;
        OFBool sawWarning = OFFalse;
        OFCHECK(runQuery(wdb, mask, results, sawWarning) == 1);
        OFCHECK(sawWarning);
        if (!results.empty())
            OFCHECK(!results.front()->tagExists(DCM_ProcedureCodeSequence));
        freeResults(results);
    }

    // ----- Test 5: option enabled, matching keys still match. -----
    // Accepting all return keys must not interfere with matching key handling.
    {
        DcmDataset mask;
        mask.putAndInsertString(DCM_PatientName, "Mustermann^Max");
        mask.insertEmptyElement(DCM_PatientTelephoneNumbers);
        OFList<DcmDataset*> results;
        OFBool sawWarning = OFFalse;
        OFCHECK(runQuery(wdb, mask, results, sawWarning) == 1);
        OFCHECK(!sawWarning);
        freeResults(results);
    }
    {
        DcmDataset mask;
        mask.putAndInsertString(DCM_PatientName, "Nomatch^Name");
        mask.insertEmptyElement(DCM_PatientTelephoneNumbers);
        OFList<DcmDataset*> results;
        OFBool sawWarning = OFFalse;
        OFCHECK(runQuery(wdb, mask, results, sawWarning) == 0);
        freeResults(results);
    }

    wdb.DisconnectFromDataSource();

    // ----- Clean up the temporary worklist database. -----
    OFStandard::deleteFile(wlFile);
    OFStandard::deleteFile(lockFile);
    removeDir(aeDir);
    removeDir(rootDir);
}
