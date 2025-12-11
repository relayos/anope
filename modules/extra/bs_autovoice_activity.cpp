/*
 * Auto-voice users based on presence duration and message count.
 *
 * (C) 2024 RelayOS
 *
 */

#include "module.h"
#include <map>
#include <set>

struct UserStats
{
	time_t first_seen;
	size_t messages;
	bool voiced;

	UserStats() : first_seen(0), messages(0), voiced(false) { }
};

class CommandBSSetAutovoice : public Command
{
	SerializableExtensibleItem<bool>& autovoice;

 public:
	CommandBSSetAutovoice(Module* creator, SerializableExtensibleItem<bool>& ext)
		: Command(creator, "botserv/set/autovoice", 2, 2)
		, autovoice(ext)
	{
		this->SetDesc(_("Enable automatic +v for active users"));
		this->SetSyntax(_("\037channel\037 {\037ON|OFF\037}"));
	}

	void Execute(CommandSource& source, const std::vector<Anope::string>& params)
	{
		ChannelInfo* ci = ChannelInfo::Find(params[0]);
		const Anope::string& value = params[1];

		if (!ci)
		{
			source.Reply(CHAN_X_NOT_REGISTERED, params[0].c_str());
			return;
		}

		if (!source.HasPriv("botserv/administration") && !source.AccessFor(ci).HasPriv("SET"))
		{
			source.Reply(ACCESS_DENIED);
			return;
		}

		if (Anope::ReadOnly)
		{
			source.Reply(_("Sorry, bot option setting is temporarily disabled."));
			return;
		}

		if (value.equals_ci("ON"))
		{
			bool override = !source.AccessFor(ci).HasPriv("SET");
			Log(override ? LOG_OVERRIDE : LOG_COMMAND, source, this, ci) << "to enable autovoice";

			autovoice.Set(ci, true);
			source.Reply(_("Auto-voice is now \002on\002 on channel %s."), ci->name.c_str());
		}
		else if (value.equals_ci("OFF"))
		{
			bool override = !source.AccessFor(ci).HasPriv("SET");
			Log(override ? LOG_OVERRIDE : LOG_COMMAND, source, this, ci) << "to disable autovoice";

			autovoice.Unset(ci);
			source.Reply(_("Auto-voice is now \002off\002 on channel %s."), ci->name.c_str());
		}
		else
			this->OnSyntaxError(source, source.command);
	}

	bool OnHelp(CommandSource& source, const Anope::string&)
	{
		this->SendSyntax(source);
		source.Reply(_(" \n"
			"Enables or disables automatic +v for users who remain in the channel\n"
			"for a configured duration and send a minimum number of messages.\n"));
		return true;
	}
};

class BSAutoVoiceActivity : public Module
{
private:
	SerializableExtensibleItem<bool> autovoice;
	CommandBSSetAutovoice command;

	/* channel name -> (user uuid -> stats) */
	std::map<Anope::string, std::map<Anope::string, UserStats> > stats;

	time_t min_duration;      // seconds
	size_t min_messages;        // messages
	bool require_account;   // only act on registered users
	std::set<Anope::string> limit_channels; // optional: if non-empty, only act on these chans

	bool ShouldTrack(Channel *c, User *u)
	{
		if (!c || !u || !c->ci || !c->ci->bi) // requires BotServ bot assigned
			return false;

		// Channel must opt-in
		if (!autovoice.HasExt(c->ci))
			return false;

		if (u->server && u->server->IsULined())
			return false; // skip services/ulined

		if (require_account && !u->Account())
			return false;

		// Optional channel allowlist
		if (!limit_channels.empty() && !limit_channels.count(c->ci->name))
			return false;

		// Skip if user already has any status
		if (c->HasUserStatus(u, "OWNER") || c->HasUserStatus(u, "PROTECT") ||
		    c->HasUserStatus(u, "OP") || c->HasUserStatus(u, "HALFOP") ||
		    c->HasUserStatus(u, "VOICE"))
			return false;

		return true;
	}

	void MaybeGrant(Channel *c, User *u, UserStats &st)
	{
		if (st.voiced)
			return;

		time_t now = Anope::CurTime;
		if (!st.first_seen)
			st.first_seen = now;

		if (st.messages < min_messages)
			return;

		if (now - st.first_seen < min_duration)
			return;

		ChannelMode *voice = ModeManager::FindChannelModeByName("VOICE");
		if (!voice)
			voice = ModeManager::FindChannelModeByChar('v');
		if (!voice)
			return;

		BotInfo *setter = c->ci->bi;
		c->SetMode(setter, voice, u->GetUID(), false);
		ModeManager::ProcessModes();

		st.voiced = true;
		Log(this) << "Auto-voiced " << u->nick << " in " << c->name
		          << " (messages=" << st.messages << ", duration=" << (now - st.first_seen) << "s)";
	}

	void TouchStats(Channel *c, User *u, bool bump_messages)
	{
		UserStats &st = stats[c->ci->name][u->GetUID()];
		if (!st.first_seen)
			st.first_seen = Anope::CurTime;
		if (bump_messages)
			++st.messages;
		MaybeGrant(c, u, st);
	}

	void Cleanup(Channel *c, User *u)
	{
		std::map<Anope::string, std::map<Anope::string, UserStats> >::iterator cit = stats.find(c->ci->name);
		if (cit == stats.end())
			return;

		cit->second.erase(u->GetUID());
		if (cit->second.empty())
			stats.erase(cit);
	}

public:
	BSAutoVoiceActivity(const Anope::string &modname, const Anope::string &creator)
		: Module(modname, creator, VENDOR)
		, autovoice(this, "BS_AUTOVOICE")
		, command(this, autovoice)
	{
		min_duration = 120;
		min_messages = 5;
		require_account = false;
	}

	void OnReload(Configuration::Conf *config)
	{
		Configuration::Block *tag = config->GetModule(this);
		min_duration = tag->Get<time_t>("min_duration", 120);
		min_messages = tag->Get<unsigned>("min_messages", 5);
		require_account = tag->Get<bool>("require_account", false);

		limit_channels.clear();
		Anope::string chans = tag->Get<const Anope::string>("channels");
		if (!chans.empty())
		{
			spacesepstream sep(chans);
			Anope::string chan;
			while (sep.GetToken(chan))
				limit_channels.insert(chan);
		}

		Log(this) << "Config: min_duration=" << min_duration
		          << "s min_messages=" << min_messages
		          << " require_account=" << (require_account ? "yes" : "no")
		          << " channels=" << (limit_channels.empty() ? "ALL" : "custom");
	}

	void OnJoinChannel(User *u, Channel *c)
	{
		if (!ShouldTrack(c, u))
			return;
		TouchStats(c, u, false);
	}

	void OnPrivmsg(User *u, Channel *c, Anope::string &)
	{
		if (!ShouldTrack(c, u))
			return;
		TouchStats(c, u, true);
	}

	void OnLeaveChannel(User *u, Channel *c)
	{
		if (!c || !c->ci)
			return;
		Cleanup(c, u);
	}

	void OnUserQuit(User *u, const Anope::string &)
	{
		if (!u)
			return;
		std::map<Anope::string, std::map<Anope::string, UserStats> >::iterator it = stats.begin();
		while (it != stats.end())
		{
			it->second.erase(u->GetUID());
			if (it->second.empty())
			{
				std::map<Anope::string, std::map<Anope::string, UserStats> >::iterator eraseme = it;
				++it;
				stats.erase(eraseme);
			}
			else
			{
				++it;
			}
		}
	}
};

MODULE_INIT(BSAutoVoiceActivity)
