/*
 *
 *  Copyright (C) 2015-2026, Open Connections GmbH
 *  All rights reserved.  See COPYRIGHT file for details.
 *
 *  This software and supporting documentation are maintained by
 *
 *    OFFIS e.V.
 *    R&D Division Health
 *    Escherweg 2
 *    D-26121 Oldenburg, Germany
 *
 *
 *  Module: dcmiod
 *
 *  Author: Michael Onken
 *
 *  Purpose: Abstract base class for IOD Modules or other attribute collections
 *
 */

#ifndef MODBASE_H
#define MODBASE_H

#include "dcmtk/config/osconfig.h"
#include "dcmtk/dcmdata/dcitem.h"
#include "dcmtk/dcmiod/ioddef.h"
#include "dcmtk/dcmiod/iodrules.h"
#include "dcmtk/ofstd/ofmem.h"

/** Class for managing sets of DICOM attributes (e.g.\ macros and modules).
 *  The data is hold in a container (DcmItem) that can be shared with other
 *  components, i.e.\ different modules that hold their data in a common
 *  container. In order to know which attributes in the container should be
 *  actually handled by a component, the component also has a set of rules, which
 *  can also be shared between different components (e.g.\ all modules of an IOD
 *  can share the same set of rules). For each attribute there is one rule in
 *  the rule set, denoting the requirement type (1,2,3,1C,2C), the VM, and
 *  besides others also the name of the component that the attribute belongs
 *  to (e.g.\ "PatientModule"). Since the component knows its own name, it can
 *  decide which attributes in the data container it is responsible for.
 *  This class is meant to support nested substructures but so far only writes
 *  attributes on a single level (including sequences). Also, in this context
 *  the class carries a parent relationship which is not used at the moment.
 *
 *  <b>Static per-class default rules (performance optimization)</b>
 *
 *  Creating many instances of an IODComponent subclass (e.g.\ CodeSequenceMacro
 *  inside per-frame functional groups of an Enhanced CT with 100,000 frames)
 *  used to allocate a fresh IODRules container and N individual IODRule objects
 *  per instance. Since the rules are usually identical for every instance of the
 *  same concrete class, this overhead is avoidable.
 *
 *  The optimization works as follows:
 *  - Each concrete subclass maintains a <em>static</em> OFshared_ptr<IODRules>
 *    (typically in an anonymous namespace in its .cc file), lazily initialized
 *    on the first instantiation of that class using a per-class OFMutex.
 *  - When resetRules() is called and no external rules container was provided
 *    (m_ExternalRules == OFFalse), the instance simply points its m_Rules
 *    shared_ptr at the class-level static. No heap allocation takes place after
 *    the very first instance of that class is created.
 *  - m_HasOwnRules is OFFalse in this state, meaning the rules are considered
 *    immutable for this instance. Any operation that needs to mutate the rule
 *    set (makeOptional(), getRules() for modification) calls ensureOwnRules()
 *    first, which performs a copy-on-write: the shared static is cloned into a
 *    new private IODRules, and m_HasOwnRules is set to OFTrue.
 *  - Calling resetRules() after makeOptional() re-points m_Rules at the
 *    static and drops the private copy (resetting to class defaults), which is
 *    the documented contract of resetRules().
 *  - When rules are provided externally via the IODComponent(item, rules,
 *    parent) constructor (m_ExternalRules == OFTrue, the pattern used by
 *    DcmIODCommon to build a single shared IODRules for all modules of an IOD),
 *    resetRules() falls back to the original behaviour and populates the shared
 *    container in place. Static defaults are still initialised on that first
 *    call so they are available for subsequent standalone instantiations.
 *
 *  How to implement resetRules() in a new subclass
 *  ------------------------------------------------
 *  The typical (optimized) pattern:
 *    1. Declare a static OFshared_ptr<IODRules> and a static OFMutex in an
 *       anonymous namespace in the .cc file.
 *    2. Lock the mutex, and if the static pointer is still null, allocate a
 *       new IODRules and add all rules to it, then unlock.
 *    3. If m_ExternalRules is OFFalse, point m_Rules at the static and set
 *       m_HasOwnRules = OFFalse (sharing path).
 *       If m_ExternalRules is OFTrue, iterate the static and clone each rule
 *       into m_Rules (in-place population path used by DcmIODCommon).
 *  See any existing resetRules() implementation for a concrete example.
 *
 *  Exception - rules that depend on constructor arguments:
 *  If the rule set varies between instances of the same class (e.g. because
 *  a constructor argument determines which rules are added, as in
 *  CodeWithModifiers), a shared static cannot be used.  In that case allocate
 *  a fresh IODRules directly into m_Rules, set m_HasOwnRules = OFTrue, and
 *  populate it normally.  The rest of the copy-on-write machinery is
 *  unaffected; the instance simply starts life already owning its rules.
 */
class DCMTK_DCMIOD_EXPORT IODComponent
{

public:
    /** Constructor
     *  @param  item The item to be used for data storage. If NULL, the
     *          class creates an empty data container.
     *  @param  rules The rule set for this class. If NULL, the class creates
     *          an empty rule set.
     *  @param  parent The parent of the IOD component (NULL if none or unknown)
     */
    IODComponent(OFshared_ptr<DcmItem> item, OFshared_ptr<IODRules> rules, IODComponent* parent = NULL);

    /** Constructor, creates rules and item from scratch.
     *  @param  parent The parent of the IOD component (NULL if none or unknown)
     */
    IODComponent(IODComponent* parent = NULL);

    /** Assignment operator, copies contained item and rule set from rhs to
     *  "this" attribute set. Performs deep copy, i.e.\ the contained item
     *  and the rule set are copied. The parent component is set to NULL.
     *  @param  rhs The IODComponent to be assigned
     *  @return Reference to this module
     */
    IODComponent& operator=(const IODComponent& rhs);

    /** Copy constructor, copies reference to contained item and
     *  rule set to "this" attribute set.
     *  @param  rhs The component to be assigned
     */
    IODComponent(const IODComponent& rhs);

    /** Virtual Destructor
     */
    virtual ~IODComponent();

    /** Clear all attributes from the data that are handled by this module.
     *  An attribute is considered belonging to the module if there are rules
     *  marked as belonging to this module via the rule's module name.
     */
    virtual void clearData();

    /** Set missing values by inventing "default values". Automatically
     *  called during write() in IODComponent. In this bas class implementation,
     *  this method checks for every rule of this component whether it has a default
     *  value, and if, invents the attribute if it is not present in the data,
     *  i.e. creating it with the default value or setting it to the default value
     *  if no value is present. This happens independently of the requirement type.
     */
    virtual void inventMissing();

    /** Resets rules to their original values
     */
    virtual void resetRules() = 0;

    /** Get rules handled by this module.
     *  Triggers copy-on-write if this instance currently shares the class-level
     *  static default rules, since the caller may intend to modify them.
     *  For read-only inspection of rules, iterate m_Rules directly from a
     *  subclass or use the const read/write helpers.
     *  @return The rules (private copy if copy-on-write was triggered)
     */
    OFshared_ptr<IODRules> getRules();

    /** Make component optional by turning all attributes requirement types of it
     *  to type 3. In order to reset to the attribute's original types,
     *  resetRules() can be used.
     */
    virtual void makeOptional();

    /** Get name of component
     *  @return Name of the module
     */
    virtual OFString getName() const = 0;

    /** Get the data handled by this module
     *  @return The item containing the data of this module
     */
    DcmItem& getData()
    {
        return *m_Item;
    }

    /** Read attributes from given item into this class
     *  @param source  The source to read from
     *  @param clearOldData If OFTrue, old data is cleared before reading. Otherwise
     *         old data is overwritten (or amended)
     *  @result EC_Normal if reading was successful, error otherwise
     */
    virtual OFCondition read(DcmItem& source, const OFBool clearOldData = OFTrue);

    /** Write attributes from this class into given item
     *  @param  destination The item to write to
     *  @result EC_Normal if writing was successful, error otherwise
     */
    virtual OFCondition write(DcmItem& destination);

    /** Check whether this component's data satisfies the underlying
     *  rules
     *  @param  quiet If OFTrue, not error / warning messages will be produced. Only
     *                the returned error code will indicate error or OK. Per default,
     *                logging output is produced (OFFalse).
     *  @result EC_Normal if rules are satisfied, error otherwise
     */
    virtual OFCondition check(const OFBool quiet = OFFalse);

    /** Comparison operator for IOD Components
     *  @param  rhs The right hand side of the comparison
     *  @return 0, if the given object is equal to this object, other value otherwise
     */
    virtual int compare(const IODComponent& rhs) const;

    /** Static helper function that reads attributes from given
     *  item into destination item, as determined by the provided
     *  rules and component name. The rules are only applied when reading
     *  from the source (which may result in warning messages on the logger),
     *  but if they could be found they are taken over into the destination
     *  item no matter whether the element validates against the rule.
     *  @param  source The item to read from
     *  @param  rules  The rules that provide the attributes and requirements
     *          for these attributes
     *  @param  destination The destination to write to
     *  @param  componentName The name of the module/component to write
     *  @result EC_Normal if reading was successful, error otherwise
     */
    static OFCondition read(DcmItem& source, IODRules& rules, DcmItem& destination, const OFString& componentName);

    /** Static helper function that writes attributes from given
     *  item into destination item, as determined by the provided
     *  rules and component name. The rules are only applied when writing
     *  to the destination (which may result in warning messages on the logger,
     *  and the whole call returning with an error). During reading from the
     *  source item the elements read are not validated against the rules.
     *  @param  source The item to read from
     *  @param  rules  The rules that provide the attributes and requirements
     *          for these attributes
     *  @param  destination The destination to write to
     *  @param  componentName The name of the module/component to write
     *  @param  checkValue If OFTrue, attribute value errors are handled as errors on writing, if OFFalse
     *          any errors are ignored.
     *  @result EC_Normal if reading was successful, error otherwise
     */
    static OFCondition write(DcmItem& source, IODRules& rules, DcmItem& destination, const OFString& componentName, const OFBool checkValue);

    /** Get whether attribute value errors will be handled as errors on writing.
     *  @return OFTrue if attribute value errors are handled as errors on writing, OFFalse otherwise.
     */
    virtual bool getValueCheckOnWrite() const;

    /** Set whether attribute values should be checked on writing, i.e. if writing
     *  should fail if attribute values violate their VR, VM, character set or value length.
     *  A missing but required value is always considered an error, independent of this setting.
     *  If set to OFFalse, writing will always succeed, even if attribute value constraints
     *  are violated. A warning instead of an error will be printed to the logger.
     *  @param  checkValue If OFTrue, attribute value errors are handled as errors on writing, if OFFalse
     *          any errors are ignored.
     */
    virtual void setValueCheckOnWrite(const OFBool checkValue);

protected:
    /** Ensure this instance has its own private, writable copy of m_Rules.
     *  If m_HasOwnRules is already OFTrue, this is a no-op. Otherwise (the
     *  instance is sharing the class-level static default rules) the shared
     *  IODRules object is cloned into a new heap-allocated copy owned solely
     *  by this instance, and m_HasOwnRules is set to OFTrue.
     *  Must be called before any in-place mutation of m_Rules (e.g. changing
     *  requirement types, adding ad-hoc rules).
     */
    void ensureOwnRules();

    /// Shared pointer to the data handled by this class. The item may contain
    /// more attributes than this class is actually responsible for
    OFshared_ptr<DcmItem> m_Item;

    /// Rules describing the attributes governed by this class.
    /// Normally points to the class-level static default rules shared by all
    /// instances (m_HasOwnRules == OFFalse). After ensureOwnRules() is called
    /// it points to a private copy (m_HasOwnRules == OFTrue).
    OFshared_ptr<IODRules> m_Rules;

    /// The parent component (may be NULL) of this class
    IODComponent* m_Parent;

    /// OFTrue if m_Rules is a private copy owned solely by this instance and
    /// safe to mutate in place. OFFalse if m_Rules is the shared class-level
    /// static and must not be modified without triggering copy-on-write first.
    OFBool m_HasOwnRules;

    /// OFTrue if the rules container was supplied externally via the
    /// IODComponent(item, rules, parent) constructor (shared-container pattern
    /// used e.g. by DcmIODCommon). When OFTrue, resetRules() populates the
    /// shared container in place instead of pointing at the per-class static.
    OFBool m_ExternalRules;

    /// Denotes whether attribute values should be checked on writing, i.e. if writing
    /// should fail if attribute values violate their VR, VM, character set or value length.
    /// A missing but required value is always considered an error, independent of this setting.
    /// If set to OFFalse, writing will always succeed, even if attribute values constraints
    /// are violated. A warning instead of an error will be printed to the logger.
    /// OFTrue if attribute value errors are handled as errors on writing, OFFalse otherwise.
    OFBool m_CheckValueOnWrite;
};

/** The class IODModule is an IODComponent without parent component since
 *  a module does always belong to the top level dataset.
 *  Also, different from IODComponents, modules usually share data and
 *  rules. This is taken into account in the assignment operator and
 *  copy constructor which only create a shallow copy, i.e. modules
 *  share the same data and rules afterwards.
 */
class DCMTK_DCMIOD_EXPORT IODModule : public IODComponent
{
public:
    /** Constructor. Similar to the one of IODComponent but no parent
     *  can be defined since a module is always at top level.
     *  @param  item The item to be used for data storage. If NULL, the
     *          class creates an empty data container.
     *  @param  rules The rule set for this class. If NULL, the class creates
     *          an empty rule set.
     */
    IODModule(OFshared_ptr<DcmItem> item, OFshared_ptr<IODRules> rules);

    /** Constructor. Creates new empty data container and new empty
     *  ruleset. No parent component is defined (since a module is always
     *  on top level.
     */
    IODModule();

    /** Copy constructor, creates shallow copy
     *  @param  rhs The module to copy from
     */
    IODModule(const IODModule& rhs);

    /** Assignment operator, creates shallow copy
     *  @param  rhs The module to copy from
     *  @return Returns reference to this object
     */
    IODModule& operator=(const IODModule& rhs);

    /** Destructor
     */
    ~IODModule() {};
};

#endif // MODBASE_H
