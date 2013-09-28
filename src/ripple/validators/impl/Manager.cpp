//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

/*

Information to track:

- Percentage of validations that the validator has signed
- Number of validations the validator signed that never got accepted


- Target number for Chosen
- Pseudo-randomly choose a subset from Chosen





Goal:

  Provide the listener with a ValidatorList.
  - This forms the UNL

Task:

  fetch ValidatorInfo array from a source

  - We have the old one and the new one, compute the following:

    * unchanged validators list
    * new validators list
    * removed validators list

  - From the unchanged / new / removed, figure out what to do.

Two important questions:

- Are there any validators in my ChosenValidators that I dont want
  * For example, they have dropped off all the trusted lists

- Do I have enough?

--------------------------------------------------------------------------------
ChosenValidators
--------------------------------------------------------------------------------

David:
  Maybe OC should have a URL that you can query to get the latest list of URI's
  for OC-approved organzations that publish lists of validators. The server and
  client can ship with that master trust URL and also the list of URI's at the
  time it's released, in case for some reason it can't pull from OC. That would
  make the default installation safe even against major changes in the
  organizations that publish validator lists.

  The difference is that if an organization that provides lists of validators
  goes rogue, administrators don't have to act.

TODO:
  Write up from end-user perspective on the deployment and administration
  of this feature, on the wiki. "DRAFT" or "PROPOSE" to mark it as provisional.
  Template: https://ripple.com/wiki/Federation_protocol
  - What to do if you're a publisher of ValidatorList
  - What to do if you're a rippled administrator
  - Overview of how ChosenValidators works

Goals:
  Make default configuration of rippled secure.
    * Ship with TrustedUriList
    * Also have a preset RankedValidators
  Eliminate administrative burden of maintaining
  Produce the ChosenValidators list.
  Allow quantitative analysis of network health.

What determines that a validator is good?
  - Are they present (i.e. sending validations)
  - Are they on the consensus ledger
  - What percentage of consensus rounds do they participate in
  - Are they stalling consensus
    * Measurements of constructive/destructive behavior is
      calculated in units of percentage of ledgers for which
      the behavior is measured.
*/

namespace ripple {
namespace Validators {

class ManagerImp
    : public Manager
    , public Stoppable
    , public Thread
    , public DeadlineTimer::Listener
    , public LeakChecked <ManagerImp>
{
public:
    Journal m_journal;
    StoreSqdb m_store;
    Logic m_logic;
    DeadlineTimer m_checkTimer;
    ServiceQueue m_queue;

    // True if we should call check on idle.
    // This gets set to false once we make it through the whole list.
    //
    bool m_checkSources;

    ManagerImp (Stoppable& parent, Journal journal)
        : Stoppable ("Validators::Manager", parent)
        , Thread ("Validators")
        , m_journal (journal)
        , m_store (m_journal)
        , m_logic (m_store, m_journal)
        , m_checkTimer (this)
        , m_checkSources (true)
    {
        m_journal.sink().set_console (true);
    }

    ~ManagerImp ()
    {
        stopThread ();
    }

    //--------------------------------------------------------------------------
    //
    // Stoppable
    //

    void onPrepare (Journal journal)
    {
        journal.info << "Preparing Validators";

        addRPCHandlers();
    }

    void onStart (Journal journal)
    {
        journal.info << "Starting Validators";

        startThread();
    }

    void onStop (Journal journal)
    {
        journal.info << "Stopping Validators";

        if (this->Thread::isThreadRunning())
            m_queue.dispatch (bind (&Thread::signalThreadShouldExit, this));
        else
            stopped();
    }

    //--------------------------------------------------------------------------
    //
    // RPC::Service
    //

    Json::Value rpcPrint (Json::Value const& args)
    {
        return m_logic.rpcPrint (args);
    }

    Json::Value rpcRebuild (Json::Value const& args)
    {
        m_queue.dispatch (bind (&Logic::buildChosen, &m_logic));
        Json::Value result;
        result ["chosen_list"] = "rebuilding";
        return result;
    }

    Json::Value rpcSources (Json::Value const& args)
    {
        return m_logic.rpcSources(args);
    }

    void addRPCHandlers()
    {
        addRPCHandler ("validators_print", beast::bind (
            &ManagerImp::rpcPrint, this, beast::_1));

        addRPCHandler ("validators_rebuild", beast::bind (
            &ManagerImp::rpcRebuild, this, beast::_1));

        addRPCHandler ("validators_sources", beast::bind (
            &ManagerImp::rpcSources, this, beast::_1));
    }

    //--------------------------------------------------------------------------

    void addStrings (String name, std::vector <std::string> const& strings)
    {
        StringArray stringArray;
        stringArray.ensureStorageAllocated (strings.size());
        for (std::size_t i = 0; i < strings.size(); ++i)
            stringArray.add (strings [i]);
        addStrings (name, stringArray);
    }

    void addStrings (String name, StringArray const& stringArray)
    {
        addStaticSource (SourceStrings::New (
            name, stringArray));
    }

    void addFile (File const& file)
    {
        addStaticSource (SourceFile::New (file));
    }

    void addURL (URL const& url)
    {
        addSource (SourceURL::New (url));
    }

    //--------------------------------------------------------------------------

    void addSource (Source* source)
    {
        m_queue.dispatch (bind (&Logic::add, &m_logic, source));
    }

    void addStaticSource (Source* source)
    {
        m_queue.dispatch (bind (&Logic::addStatic, &m_logic, source));
    }

    // VFALCO NOTE we should just do this on the callers thread?
    //
    void receiveValidation (ReceivedValidation const& rv)
    {
#if RIPPLE_USE_NEW_VALIDATORS
        if (! isStopping())
            m_queue.dispatch (bind (
                &Logic::receiveValidation, &m_logic, rv));
#endif
    }

    // VFALCO NOTE we should just do this on the callers thread?
    //
    void ledgerClosed (RippleLedgerHash const& ledgerHash)
    {
#if RIPPLE_USE_NEW_VALIDATORS
        if (! isStopping())
            m_queue.dispatch (bind (
                &Logic::ledgerClosed, &m_logic, ledgerHash));
#endif
    }

    //--------------------------------------------------------------------------

    void init ()
    {
        m_journal.trace << "Initializing";

        File const file (File::getSpecialLocation (
            File::userDocumentsDirectory).getChildFile ("validators.sqlite"));
        
        m_journal.trace << "Opening database at '" << file.getFullPathName() << "'";

        Error error (m_store.open (file));

        if (error)
        {
            m_journal.fatal <<
                "Failed to open '" << file.getFullPathName() << "'";
        }

        if (! error)
        {
            m_logic.load ();
        }
    }

    void onDeadlineTimer (DeadlineTimer& timer)
    {
        if (timer == m_checkTimer)
        {
            m_journal.trace << "Check timer signaled";
            m_queue.dispatch (bind (&ManagerImp::setCheckSources, this));
        }
    }

    void setCheckSources ()
    {
        m_checkSources = true;
    }

    void checkSources ()
    {
        if (m_checkSources)
        {
            m_journal.trace << "Checking sources";

            if (m_logic.fetch_one () == 0)
            {
                m_journal.trace << "Finished checking sources";

                // Made it through the list without interruption!
                // Clear the flag and set the deadline timer again.
                //
                m_checkSources = false;

                m_journal.trace << "Next check timer expires in " <<
                    RelativeTime::seconds (checkEverySeconds);

                m_checkTimer.setExpiration (checkEverySeconds);
            }
        }
    }

    void run ()
    {
        init ();

        while (! this->threadShouldExit())
        {
            checkSources ();
            m_queue.run_one();
        }

        stopped();
    }
};

//------------------------------------------------------------------------------

Validators::Manager* Validators::Manager::New (Stoppable& parent, Journal journal)
{
    return new Validators::ManagerImp (parent, journal);
}

}
}
