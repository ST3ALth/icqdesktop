#include "stdafx.h"
#include "ContactListModel.h"
#include "../../cache/avatars/AvatarStorage.h"

#include "../history_control/MessagesModel.h"
#include "../../core_dispatcher.h"
#include "../../gui_settings.h"
#include "../../utils/profiling/auto_stop_watch.h"
#include "contact_profile.h"
#include "../../utils/utils.h"
#include "../../core_dispatcher.h"
#include "../../utils/InterConnector.h"
#include "../../utils/gui_coll_helper.h"
#include "../../my_info.h"
#include "RecentsModel.h"
#include "../history_control/HistoryControlPage.h"
#include "../MainWindow.h"
#include "../../controls/GeneralDialog.h"
#include "Common.h"

namespace
{
    struct ItemLessThan
    {
        inline bool operator() (const Logic::ContactItem& first, const Logic::ContactItem& second)
        {
            if (first.Get()->GroupId_ == second.Get()->GroupId_)
            {
                if (first.is_group() && second.is_group())
                    return false;

                if (first.is_group())
                    return true;

                if (second.is_group())
                    return false;

                return first.Get()->GetDisplayName().toUpper() < second.Get()->GetDisplayName().toUpper();
            }

            return first.Get()->GroupId_ < second.Get()->GroupId_;
        }
    };

    bool IsActiveContact(const Logic::ContactItem &contact)
    {
        return (contact.is_online() || contact.recently() || contact.Get()->Is_chat_) && !contact.is_group();
    }

    struct ItemLessThanNoGroups
    {
        inline bool operator() (const Logic::ContactItem& first, const Logic::ContactItem& second)
        {
            if (first.Get()->IsChecked_ != second.Get()->IsChecked_)
                return first.Get()->IsChecked_;

            if (IsActiveContact(first) != IsActiveContact(second))
                return IsActiveContact(first);

            return first.Get()->GetDisplayName().toUpper() < second.Get()->GetDisplayName().toUpper();
        }
    };

    struct ItemLessThanSelectMembers
    {
        inline bool operator() (const Logic::ContactItem& first, const Logic::ContactItem& second)
        {
            if (first.Get()->IsChecked_ != second.Get()->IsChecked_)
                return first.Get()->IsChecked_;

            if (IsActiveContact(first) != IsActiveContact(second))
                return IsActiveContact(first);

            const auto& first_name = first.Get()->GetDisplayName().toUpper();
            const auto& second_name = second.Get()->GetDisplayName().toUpper();

            if (first_name[0].isLetter() != second_name[0].isLetter())
                return first_name[0].isLetter();

            std::function<bool(QChar)> is_latin = [](QChar _c){return ((_c >= 'a' && _c <= 'z') || (_c >= 'A' && _c <= 'Z'));};

            if (is_latin(first_name[0]) != is_latin(second_name[0]))
                return !is_latin(first_name[0]);

            return first_name < second_name;
        }
    };

    const int REFRESH_TIMER = 5000;
}

namespace Logic
{
    std::unique_ptr<ContactListModel> g_contact_list_model;

    ContactListModel::ContactListModel(QObject *parent)
        : CustomAbstractListModel(parent)
        , scrollPosition_(0)
        , minVisibleIndex_(0)
        , maxVisibleIndex_(0)
        , ref_(new bool(false))
        , timer_(new QTimer(this))
        , searchRequested_(false)
        , isWithCheckedBox_(false)
        , gotPageCallback_(nullptr)
    {
        // !!!temporary, on dlg_state engine must add contact to contactlist and send changes to gui
        connect(Ui::GetDispatcher(), SIGNAL(dlgState(Data::DlgState)), this, SLOT(dlgState(Data::DlgState)), Qt::QueuedConnection);

        connect(Ui::GetDispatcher(), SIGNAL(contactList(std::shared_ptr<Data::ContactList>, QString)), this, SLOT(contactList(std::shared_ptr<Data::ContactList>, QString)), Qt::QueuedConnection);
        connect(Ui::GetDispatcher(), SIGNAL(presense(Data::Buddy*)), this, SLOT(presense(Data::Buddy*)), Qt::QueuedConnection);
        connect(Ui::GetDispatcher(), SIGNAL(contactRemoved(QString)), this, SLOT(contactRemoved(QString)), Qt::QueuedConnection);
        connect(GetAvatarStorage(), SIGNAL(avatarChanged(QString)), this, SLOT(avatarLoaded(QString)), Qt::QueuedConnection);
        connect(Ui::GetDispatcher(), SIGNAL(searchResult(QStringList)), this, SLOT(searchResult(QStringList)), Qt::QueuedConnection);
        connect(Ui::GetDispatcher(), SIGNAL(chatInfo(qint64, std::shared_ptr<Data::ChatInfo>)), this, SLOT(chatInfo(qint64, std::shared_ptr<Data::ChatInfo>)), Qt::QueuedConnection);

        timer_->setSingleShot(true);
        timer_->setInterval(REFRESH_TIMER);
        connect(timer_, &QTimer::timeout, this, &Logic::ContactListModel::refresh, Qt::QueuedConnection);

        connect(&Utils::InterConnector::instance(), &Utils::InterConnector::profileSettingsUnknownAdd,
            this, &Logic::ContactListModel::auth_add_contact, Qt::QueuedConnection);
        connect(&Utils::InterConnector::instance(), &Utils::InterConnector::profileSettingsUnknownAdd,
            this, &Logic::ContactListModel::stats_auth_add_contact, Qt::QueuedConnection);
        connect(&Utils::InterConnector::instance(), &Utils::InterConnector::profileSettingsUnknownSpam,
            this, &Logic::ContactListModel::unknown_contact_profile_spam_contact, Qt::QueuedConnection);
        connect(&Utils::InterConnector::instance(), &Utils::InterConnector::profileSettingsUnknownSpam, 
            this, &Logic::ContactListModel::stats_spam_profile, Qt::QueuedConnection);
        connect(&Utils::InterConnector::instance(), &Utils::InterConnector::profileSettingsUnknownIgnore,
            this, &Logic::ContactListModel::auth_ignore_contact, Qt::QueuedConnection);
    }

    int ContactListModel::rowCount(const QModelIndex &) const
    {
        int visibleCount = -1;
        getAbsIndexByVisibleIndex((int)contacts_.size(), &visibleCount, (int)contacts_.size());
        return visibleCount + 1;
    }

    int ContactListModel::getAbsIndexByVisibleIndex(int cur, int* visibleCount, int iter_limit) const
    {
        bool groups_enabled = Ui::get_gui_settings()->get_value<bool>(settings_cl_groups_enabled, false);
        int iter = 0;
        *visibleCount = -1;
        for (;*visibleCount < cur && (unsigned)iter < contacts_.size() && iter < iter_limit; ++iter)
        {
            if (contacts_[iter].is_visible() && (groups_enabled || !contacts_[iter].is_group()))
            {
                ++*visibleCount;
            }
        }
        return iter;
    }

    QModelIndex ContactListModel::contactIndex(const QString& aimId)
    {
        int visibleCount = 0;
        auto contact_index = indexes_.find(aimId);
        getAbsIndexByVisibleIndex(contact_index.value(), &visibleCount, contact_index.value());

        return index(visibleCount + 1);
    }

    QVariant ContactListModel::data(const QModelIndex &i, int r) const
    {
        int cur = i.row();

        if (!i.isValid() || (unsigned)cur >= contacts_.size())
        {
            return QVariant();
        }

        if (r == Qt::DisplayRole)
        {
            if (maxVisibleIndex_ == 0 && minVisibleIndex_ == 0)
                maxVisibleIndex_ = minVisibleIndex_ = cur;

            if (cur < minVisibleIndex_)
                minVisibleIndex_ = cur;
            if (cur > maxVisibleIndex_)
                maxVisibleIndex_ = cur;
        }

        int visibleCount = -1;
        auto iter = getAbsIndexByVisibleIndex(cur, &visibleCount, (int)contacts_.size());
        auto cont = contacts_[iter - 1].Get();

        if (Testing::isAccessibleRole(r))
            return cont->AimId_;

        return QVariant::fromValue(cont);
    }

    QString ContactListModel::contactToTryOnTheme() const
    {
        if (!currentAimdId_.isEmpty())
            return currentAimdId_;

        QString recentsFirstContact = Logic::GetRecentsModel()->firstContact();
        if (!recentsFirstContact.isEmpty())
            return recentsFirstContact;

        if (!contacts_.empty())
            return contacts_.front().get_aimid();

        return QString();
    }

    Qt::ItemFlags ContactListModel::flags(const QModelIndex &i) const
    {
        if (!i.isValid())
            return Qt::ItemIsEnabled;

        unsigned flags = QAbstractItemModel::flags(i) | Qt::ItemIsEnabled;

        int visibleCount = -1;
        auto iter = getAbsIndexByVisibleIndex(i.row(), &visibleCount, (int)contacts_.size());
        auto cont = contacts_[iter - 1].Get();

        if (i.row() == 0 || cont->GetType() == Data::GROUP)
            flags |= ~Qt::ItemIsSelectable;
        flags |= ~Qt::ItemIsEditable;

        return (Qt::ItemFlags)flags;
    }

    void ContactListModel::searchResult(QStringList result)
    {
        unsigned size = (unsigned)match_.size();
        match_ = GetContactListModel()->getSearchedContacts(result.toStdList());
        emit dataChanged(index(0), index(size));
        if (!result.isEmpty())
            emit results();
    }

    bool ContactListModel::isShowInSelectMembers(const ContactItem& item)
    {
        return (!isWithCheckedBox_ || (!item.is_chat() && Ui::MyInfo()->aimId() != item.get_aimid()));
    }

    std::vector<ContactItem> ContactListModel::getSearchedContacts(std::list<QString> contacts)
    {
        std::vector<ContactItem> result = GetCheckedContacts();		
        for (const auto &contact : contacts)
        {
            auto item = contacts_[indexes_.value(contact)];
            if (!item.is_group() && !item.is_checked())
            {
                if (isShowInSelectMembers(item))
                    result.emplace_back(item);
            }
        }

        return result;
    }

    std::vector<ContactItem> ContactListModel::getSearchedContacts()
    {
        // return GetSearchedContacts(contacts);

        std::vector<ContactItem> result = GetCheckedContacts();
        for (auto iter = contacts_.begin(); iter != contacts_.end(); ++iter)
        {
            if (!iter->is_group() && !iter->is_checked())
            {
                if (isShowInSelectMembers(*iter))
                    result.emplace_back(*iter);
            }
        }

        return result;
    }

    void ContactListModel::searchPatternChanged(QString p)
    {
        searchPatterns_ = Utils::GetPossibleStrings(p);
        if (p.isEmpty())
        {
            unsigned size = (unsigned)match_.size();
            match_ = GetContactListModel()->getSearchedContacts();
            emit dataChanged(index(0), index(size));
            return;
        }

        if (!searchRequested_)
        {
            QTimer::singleShot(200, [this]()
            {
                searchRequested_ = false;
                Ui::gui_coll_helper collection(Ui::GetDispatcher()->create_collection(), true);
                core::ifptr<core::iarray> patternsArray(collection->create_array());
                patternsArray->reserve(searchPatterns_.size());
                for (auto iter = searchPatterns_.begin(); iter != searchPatterns_.end(); ++iter)
                {
                    core::coll_helper coll(collection->create_collection(), true);
                    coll.set_value_as_string("pattern", iter->toUtf8().data(), iter->toUtf8().size());
                    core::ifptr<core::ivalue> val(collection->create_value());
                    val->set_as_collection(coll.get());
                    patternsArray->push_back(val.get());
                }
                collection.set_value_as_array("search_patterns", patternsArray.get());
                Ui::GetDispatcher()->post_message_to_core("search", collection.get());
            });
            searchRequested_ = true;
        }
    }

    void ContactListModel::setFocus()
    {
        match_.clear();
    }

    int ContactListModel::addItem(Data::Contact *contact)
    {
        {
            int visibleCount = -1;
            getAbsIndexByVisibleIndex((int)contacts_.size(), &visibleCount, (int)contacts_.size());
            if ((visibleCount + 1) <= 1)
                emit Utils::InterConnector::instance().hideNoContactsYet();
        }

        auto item = getContactItem(contact->AimId_);
        if (item != nullptr)
        {
            item->Get()->ApplyBuddy(contact);
            Logic::GetAvatarStorage()->UpdateDefaultAvatarIfNeed(contact->AimId_);
        }
        else
        {
            contacts_.emplace_back(contact);
            indexes_.insert(contact->AimId_, (int)contacts_.size() - 1);
        }

        return (int)contacts_.size();
    }

    void ContactListModel::rebuild_index()
    {
        indexes_.clear();
        unsigned i = 0;
        for (const auto &contact : contacts_)
        {
            indexes_.insert(contact.Get()->AimId_, i);
            ++i;
        }
    }

    void ContactListModel::contactList(std::shared_ptr<Data::ContactList> _cl, QString type)
    {
        const int size = (int)contacts_.size();
        beginInsertRows(QModelIndex(), size, _cl->size() + size);

        for (auto iter : _cl->uniqueKeys())
        {
            if (type != "deleted")
            {
                if (iter->UserType_ != "sms")
                {
                    addItem(iter);
                    if (iter->IsLiveChat_)
                        emit liveChatJoined(iter->AimId_);
                    emit contactChanged(iter->AimId_);
                }
            }
            else
            {
                QString aimid = iter->AimId_;
                auto cont = std::find_if(contacts_.begin(), contacts_.end(), [aimid](const ContactItem& item) { return item.Get()->AimId_ == aimid; });
                if (cont != contacts_.end())
                {
                    if (cont->is_live_chat())
                        emit liveChatRemoved(aimid);
                    contacts_.erase(cont);
                }
                emit contact_removed(aimid);
            }
        }

        std::list<int> groupIds;
        for (auto iter : *_cl)
        {
            if (std::find(groupIds.begin(), groupIds.end(), iter->Id_) != groupIds.end())
                continue;

            if (type != "deleted")
            {
                groupIds.push_back(iter->Id_);
                Data::Group* group = new Data::Group();
                group->ApplyBuddy(iter);
                addItem(group);
            }
            else if (iter->Removed_)
            {
                int id = iter->Id_;
                contacts_.erase(std::remove_if(contacts_.begin(), contacts_.end(), [id](const ContactItem& item) { return item.Get()->GroupId_ == id; }), contacts_.end());
            }
        }

        sort();
        endInsertRows();

        rebuild_index();

        {
            int visibleCount = -1;
            getAbsIndexByVisibleIndex((int)contacts_.size(), &visibleCount, (int)contacts_.size());
            if ((visibleCount + 1) == 0)
            {
                emit Utils::InterConnector::instance().showNoContactsYet();
                emit Utils::InterConnector::instance().showNoRecentsYet();
            }
            else if (size == 0)
                emit Utils::InterConnector::instance().hideNoContactsYet();
        }
    }

    void ContactListModel::dlgState(Data::DlgState _dlgState)
    {
        if (!getContactItem(_dlgState.AimId_))
        {
            std::unique_ptr<Data::Contact> addContact(new Data::Contact());
            addContact->AimId_ = _dlgState.AimId_;
            addContact->State_ = "";
            addContact->NotAuth_ = true;
            addContact->Friendly_ = _dlgState.Friendly_;
            addContact->Is_chat_ = _dlgState.Chat_;
            addItem(addContact.release());
        }
    }

    void ContactListModel::contactRemoved(QString contact)
    {
        contacts_.erase(std::remove_if(contacts_.begin(), contacts_.end(), [contact](const ContactItem& item) { return item.Get()->AimId_ == contact; }), contacts_.end());
        rebuild_index();
        emit contact_removed(contact);
        {
            int visibleCount = -1;
            getAbsIndexByVisibleIndex((int)contacts_.size(), &visibleCount, (int)contacts_.size());
            emit Utils::InterConnector::instance().showPlaceholder(Utils::PlaceholdersType::PlaceholdersType_IntroduceYourself);

            if ((visibleCount + 1) == 0)
                emit Utils::InterConnector::instance().showNoContactsYet();
        }
    }

    void ContactListModel::refresh()
    {
        sort();
        rebuild_index();
        for (int i = minVisibleIndex_; i <= maxVisibleIndex_; ++i)
            emit dataChanged(index(i), index(i));
    }

    void ContactListModel::refreshList()
    {
        for (int i = minVisibleIndex_; i <= maxVisibleIndex_; ++i)
            emit dataChanged(index(i), index(i));
    }

    void ContactListModel::avatarLoaded(QString aimId)
    {
        QHash<QString, int>::const_iterator iter = indexes_.find(aimId);
        if (iter != indexes_.end())
        {
            emit dataChanged(index(iter.value()), index(iter.value()));
        }
    }

    void ContactListModel::presense(Data::Buddy* presence)
    {
        QHash<QString, int>::const_iterator iter = indexes_.find(presence->AimId_);
        if (iter != indexes_.end())
        {
            auto iconId = contacts_[iter.value()].Get()->iconId_;
            auto bigIconId = contacts_[iter.value()].Get()->bigIconId_;
            auto largeIconId = contacts_[iter.value()].Get()->largeIconId_;
            if (/*presence->iconId_ != iconId || presence->bigIconId_ != bigIconId || */presence->largeIconId_ != largeIconId) // large icon would be enough
            {
                GetAvatarStorage()->UpdateAvatar(presence->AimId_);
            }
            
            contacts_[iter.value()].Get()->ApplyBuddy(presence);
            pushChange(iter.value());
            emit contactChanged(presence->AimId_);
            if (!timer_->isActive())
                timer_->start();
        }
    }

    void ContactListModel::groupClicked(int groupId)
    {
        auto contact_index = 0;

        for (auto &contact : contacts_)
        {
            if (contact.Get()->GroupId_ == groupId && !contact.is_group())
            {
                contact.set_visible(!contact.is_visible());
                emit dataChanged(index(contact_index), index(contact_index));
            }

            ++contact_index;
        }
    }

    void ContactListModel::scrolled(int value)
    {
        minVisibleIndex_ = 0;
        maxVisibleIndex_ = 0;
        scrollPosition_ = value;
    }

    void ContactListModel::pushChange(int i)
    {
        updatedItems_.push_back(i);
        int scrollPos = scrollPosition_;
        QTimer::singleShot(2000, [this, scrollPos](){ if (scrollPosition_ == scrollPos) processChanges(); });
    }

    void ContactListModel::processChanges()
    {
        if (updatedItems_.empty())
            return;

        for (auto iter : updatedItems_)
        {
            if (iter >= minVisibleIndex_ && iter <= maxVisibleIndex_)
                emit dataChanged(index(iter), index(iter));
        }

        updatedItems_.clear();
    }

    void ContactListModel::setCurrent(QString _aimId, bool sel, bool needSwitchTab, std::function<void(Ui::HistoryControlPage*)> _gotPageCallback)
    {
        if (_gotPageCallback)
        {
            auto page = Utils::InterConnector::instance().getMainWindow()->getHistoryPage(_aimId);
            if (page)
            {
                _gotPageCallback(page);
            }
            else
            {
                gotPageCallback_ = _gotPageCallback;
            }
        }

        currentAimdId_ = _aimId;
        if (sel && !currentAimdId_.isEmpty())
            emit select(currentAimdId_);
        else
            emit selectedContactChanged(currentAimdId_);

        if (needSwitchTab)
            emit switchTab(_aimId);
    }

    void ContactListModel::setCurrentCallbackHappened(Ui::HistoryControlPage *page)
    {
        if (gotPageCallback_)
        {
            gotPageCallback_(page);
            gotPageCallback_ = nullptr;
        }
    }

    void ContactListModel::sort()
    {
        if (!contacts_.size())
            return;

        std::function<bool (const Logic::ContactItem&, const Logic::ContactItem&)> less = ItemLessThanNoGroups();

        // if (GetIsWithCheckedBox())
        // 	less = ItemLessThanSelectMembers();
        // else
        if (Ui::get_gui_settings()->get_value<bool>(settings_cl_groups_enabled, false))
            less = ItemLessThan();
        else
            less = ItemLessThanNoGroups();

        std::sort(contacts_.begin(), contacts_.end(), less);
    }

    QString ContactListModel::selectedContact() const
    {
        return currentAimdId_;
    }

    QString ContactListModel::selectedContactName() const
    {
        auto contact = getContactItem(currentAimdId_);
        return contact == nullptr ? QString() : contact->Get()->GetDisplayName();
    }

    QString ContactListModel::selectedContactState() const
    {
        auto contact = getContactItem(currentAimdId_);
        return contact == nullptr ? QString() : contact->Get()->State_;
    }

    ContactItem* ContactListModel::getContactItem(const QString& _aimId)
    {
        return const_cast<ContactItem*>(static_cast<const ContactListModel*>(this)->getContactItem(_aimId));
    }

    const ContactItem* ContactListModel::getContactItem(const QString& _aimId) const
    {
        auto iter = indexes_.find(_aimId);
        if (iter == indexes_.end())
            return nullptr;

        int idx = iter.value();
        if (idx >= (int) contacts_.size())
            return nullptr;

        return &contacts_[idx];
	}

	QString ContactListModel::getDisplayName(const QString& aimId) const
	{
		auto ci = getContactItem(aimId);
		if (!ci)
		{
			return aimId;
		}

		return ci->Get()->GetDisplayName();
	}

	bool ContactListModel::isChat(const QString& aimId) const
	{
		auto ci = getContactItem(aimId);
		if (!ci)
		{
			return false;
		}

		return ci->is_chat();
	}

	bool ContactListModel::isMuted(const QString& aimId) const
	{
		auto ci = getContactItem(aimId);
		if (!ci)
		{
			return false;
		}

		return ci->is_muted();
	}

    bool ContactListModel::isLiveChat(const QString& aimId) const
    {
        auto ci = getContactItem(aimId);
        if (!ci)
        {
            return false;
        }

        return ci->is_live_chat();
    }

    bool ContactListModel::isOfficial(const QString& aimId) const
    {
        auto ci = getContactItem(aimId);
        if (!ci)
        {
            return false;
        }

        return ci->is_official();
    }

    bool ContactListModel::isNotAuth(const QString& aimId) const
    {
        auto ci = getContactItem(aimId);
        if (!ci)
        {
            return false;
        }

        return ci->is_not_auth();
    }

    QString ContactListModel::getState(const QString& aimId) const
    {
        auto ci = getContactItem(aimId);
        if (!ci)
        {
            return "";
        }
		return ci->Get()->GetState();
	}

    QDateTime ContactListModel::getLastSeen(const QString& aimId) const
    {
        auto ci = getContactItem(aimId);
        if (!ci)
        {
            return QDateTime::currentDateTime();
        }

        return ci->Get()->GetLastSeen();
    }

    ContactListModel* GetContactListModel()
    {
        if (!g_contact_list_model)
            g_contact_list_model.reset(new ContactListModel(0));

        return g_contact_list_model.get();
    }

    void ResetContactListModel()
    {
        if (g_contact_list_model)
            g_contact_list_model.reset();
    }

    QString ContactListModel::getInputText(const QString& aimId) const
    {
        auto ci = getContactItem(aimId);
        if (!ci)
        {
            return "";
        }

        return ci->get_input_text();
    }

    void ContactListModel::setInputText(const QString& aimId, const QString& _text)
    {
        auto ci = getContactItem(aimId);
        if (!ci)
        {
            return;
        }

        ci->set_input_text(_text);
    }

    void ContactListModel::get_contact_profile(const QString& _aimId, std::function<void(profile_ptr, int32_t error)> _call_back)
    {
        profile_ptr profile; 

        if (!_aimId.isEmpty())
        {
            auto ci = getContactItem(_aimId);

            if (ci)
            {
                profile = ci->get_contact_profile();
            }
        }

        if (profile)
        {
            emit profile_loaded(profile);

            _call_back(profile, 0);
        }

        if (!profile)
        {
            Ui::gui_coll_helper collection(Ui::GetDispatcher()->create_collection(), true);

            collection.set_value_as_qstring("aimid", _aimId);

            std::weak_ptr<bool> wr_ref = ref_;

            Ui::GetDispatcher()->post_message_to_core("contacts/profile/get", collection.get(), [this, _call_back, wr_ref](core::icollection* _coll)
            {
                auto ref = wr_ref.lock();
                if (!ref)
                    return;

                Ui::gui_coll_helper coll(_coll, false);

                QString aimid = coll.get_value_as_string("aimid");
                int32_t err = coll.get_value_as_int("error");

                if (err == 0)
                {
                    Ui::gui_coll_helper coll_profile(coll.get_value_as_collection("profile"), false);

                    profile_ptr profile(new contact_profile());

                    if (profile->unserialize(coll_profile))
                    {
                        if (!aimid.isEmpty())
                        {
                            ContactItem* ci = getContactItem(aimid);
                            if (ci)
                                ci->set_contact_profile(profile);
                        }

                        emit profile_loaded(profile);

                        _call_back(profile, 0);
                    }
                }
                else
                {
                    _call_back(nullptr, 0);
                }
            });
        }
    }

    void ContactListModel::add_contact_to_contact_list(const QString& _aimid, std::function<void(bool)> _call_back)
    {
        Ui::gui_coll_helper collection(Ui::GetDispatcher()->create_collection(), true);
        collection.set_value_as_qstring("contact", _aimid);
        collection.set_value_as_qstring("group", "General");
        collection.set_value_as_qstring("message", QT_TRANSLATE_NOOP("contact_list","Hello. Please add me to your contact list"));

        std::weak_ptr<bool> wr_ref = ref_;

        Ui::GetDispatcher()->post_message_to_core("contacts/add", collection.get(), [this, _aimid, _call_back, wr_ref](core::icollection* _coll)
        {
            auto ref = wr_ref.lock();
            if (!ref)
                return;

            Logic::ContactItem* item = getContactItem(_aimid);
            if (item && item->is_not_auth())
            {
                item->reset_not_auth();
                emit contactChanged(_aimid);
            }

            Ui::gui_coll_helper coll(_coll, false);

            QString contact = coll.get_value_as_string("contact");
            int32_t err = coll.get_value_as_int("error");

            _call_back(err == 0);
            emit contact_added(contact, (err == 0));
            emit needSwitchToRecents();
        });
    }

    void ContactListModel::rename_contact(const QString& _aimid, const QString& _friendly)
    {
        Ui::gui_coll_helper collection(Ui::GetDispatcher()->create_collection(), true);

        collection.set_value_as_qstring("contact", _aimid);
        collection.set_value_as_qstring("friendly", _friendly);

        Ui::GetDispatcher()->post_message_to_core("contacts/rename", collection.get());
    }

    void ContactListModel::rename_chat(const QString& _aimid, const QString& _friendly)
    {
        Ui::gui_coll_helper collection(Ui::GetDispatcher()->create_collection(), true);

        collection.set_value_as_qstring("aimid", _aimid);
        collection.set_value_as_qstring("m_chat_name", _friendly);
        Ui::GetDispatcher()->post_message_to_core("modify_chat", collection.get());
    }

    void ContactListModel::remove_contact_from_contact_list(const QString& _aimid)
    {
        Ui::gui_coll_helper collection(Ui::GetDispatcher()->create_collection(), true);
        collection.set_value_as_qstring("contact", _aimid);
        Ui::GetDispatcher()->post_message_to_core("contacts/remove", collection.get());
    }

    bool ContactListModel::block_spam_contact(const QString& _aimid, bool _with_confirmation /*= true*/)
    {
        bool confirm = false;
        if (_with_confirmation)
        {
            confirm = Utils::GetConfirmationWithTwoButtons(
                QT_TRANSLATE_NOOP("popup_window", "Cancel"),
                QT_TRANSLATE_NOOP("popup_window", "Yes"),
                QT_TRANSLATE_NOOP("popup_window", "Are you sure this contact is spam?"),
                Logic::GetContactListModel()->getDisplayName(_aimid),
                NULL);
        }

        if (confirm || !_with_confirmation)
        {
            Ui::gui_coll_helper collection(Ui::GetDispatcher()->create_collection(), true);
            collection.set_value_as_qstring("contact", _aimid);
            Ui::GetDispatcher()->post_message_to_core("contacts/block", collection.get());
        }
        return confirm;
    }

    void ContactListModel::ignore_contact(const QString& _aimid, bool ignore)
    {
        Ui::gui_coll_helper collection(Ui::GetDispatcher()->create_collection(), true);
        collection.set_value_as_qstring("contact", _aimid);
        collection.set_value_as_bool("ignore", ignore);
        Ui::GetDispatcher()->post_message_to_core("contacts/ignore", collection.get());

		if (ignore)
		{
			emit ignoreContact(_aimid);
		}
    }

    bool ContactListModel::ignore_and_remove_from_cl_contact(const QString& _aimid)
    {
        auto confirm = Utils::GetConfirmationWithTwoButtons(
            QT_TRANSLATE_NOOP("popup_window", "Cancel"),
            QT_TRANSLATE_NOOP("popup_window", "Yes"),
            QT_TRANSLATE_NOOP("popup_window", "Are you sure you want to move contact to ignore list?"),
            Logic::GetContactListModel()->getDisplayName(_aimid),
            NULL);

        if (confirm)
        {
            Logic::GetContactListModel()->ignore_contact(_aimid, true);
            Logic::GetContactListModel()->remove_contact_from_contact_list(_aimid);
        }
        return confirm;
    }

    bool ContactListModel::isYouAdmin(const QString& aimId)
    {
        auto cont = getContactItem(aimId);
        if (cont)
            return cont->is_you_admin();

        return false;
    }

    void ContactListModel::get_ignore_list()
    {
        Ui::gui_coll_helper collection(Ui::GetDispatcher()->create_collection(), true);
        Ui::GetDispatcher()->post_message_to_core("contacts/get_ignore", collection.get());
    }

    void ContactListModel::emitChanged(int first, int last)
    {
        emit dataChanged(index(first), index(last));
    }

    void ContactListModel::ChangeChecked(const QString &_aimid)
    {
        auto item = getContactItem(_aimid);
        item->set_checked(!item->is_checked());
    }

    std::vector<ContactItem> ContactListModel::GetCheckedContacts() const
    {
        std::vector<ContactItem> result;
        for (const auto& item : contacts_)
        {
            if (item.is_checked())
            {
                result.push_back(item);
            }
        }
        return result;
    }

    void ContactListModel::clearChecked()
    {
        for (auto item : contacts_)
        {
            item.set_checked(false);
        }
    }

    void ContactListModel::setChecked(QString& _aimid)
    {
        getContactItem(_aimid)->set_checked(true);
    }

    bool ContactListModel::setIsWithCheckedBox()
    {
        return isWithCheckedBox_;
    }

    void ContactListModel::setIsWithCheckedBox(bool _isWithCheckedBox)
    {
        isWithCheckedBox_ = _isWithCheckedBox;
    }

    void ContactListModel::auth_add_contact(QString _aimid)
    {
        add_contact_to_contact_list(_aimid);
        // connect(Logic::GetContactListModel(), SIGNAL(contact_added(QString, bool)), this, SLOT(contact_authorized(QString, bool)));
    }

    void ContactListModel::stats_auth_add_contact(QString _aimid)
    {
        Ui::GetDispatcher()->post_stats_to_core(core::stats::stats_event_names::add_user_dialog);
    }

    void ContactListModel::unknown_contact_profile_spam_contact(QString _aimid)
    {
        if (block_spam_contact(_aimid))
            emit Utils::InterConnector::instance().profileSettingsBack();
    }

    void ContactListModel::auth_spam_contact(QString _aimid)
    {
        block_spam_contact(_aimid, false);

        emit Utils::InterConnector::instance().profileSettingsBack();
    }

    void ContactListModel::auth_delete_contact(QString _aimid)
    {
        remove_contact_from_contact_list(_aimid);

        Ui::gui_coll_helper collection(Ui::GetDispatcher()->create_collection(), true);
        collection.set_value_as_qstring("contact", _aimid);
        Ui::GetDispatcher()->post_message_to_core("dialogs/hide", collection.get());
    }

    void ContactListModel::auth_ignore_contact(QString _aimid)
    {
        if (ignore_and_remove_from_cl_contact(_aimid))
        {
            Ui::GetDispatcher()->post_stats_to_core(core::stats::stats_event_names::ignore_profile_page);
            emit Utils::InterConnector::instance().profileSettingsBack();
        }
    }

    void ContactListModel::stats_spam_profile(QString _aimid)
    {
        Ui::GetDispatcher()->post_stats_to_core(core::stats::stats_event_names::spam_profile_page);
    }

    void ContactListModel::chatInfo(qint64, std::shared_ptr<Data::ChatInfo> info)
    {
        auto iter = indexes_.find(info->AimId_);
        if (iter == indexes_.end())
            return;

        int idx = iter.value();
        if (idx >= (int) contacts_.size())
            return;

        contacts_[idx].set_chat_admin(info->YourRole_ == "admin" || info->YourRole_ == "moder");
    }


    void ContactListModel::joinLiveChat(const QString& _stamp, bool _silent)
    {
        if (_silent)
        {
            Ui::gui_coll_helper collection(Ui::GetDispatcher()->create_collection(), true);

            collection.set_value_as_qstring("stamp", _stamp);

            Ui::GetDispatcher()->post_message_to_core("livechat/join", collection.get());
        }
        else
        {
            emit needJoinLiveChat(_stamp);
        }
    }
}