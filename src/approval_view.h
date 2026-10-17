#pragma once
#include "core.h"
#include <algorithm>
namespace chuan {
struct ApprovalRow { bool sender=false; std::wstring peer,name; uint64_t request=0,total=0; size_t item=0,count=0; bool folder=false; };
struct ApprovalView {
    std::map<std::pair<uint64_t,size_t>,bool> checked;
    std::set<std::wstring> collapsed;
    std::vector<ApprovalRow> rows,leaves;
    void Update(const std::vector<Transfer> &requests) {
        leaves.clear();std::vector<std::wstring> senders;std::set<std::pair<uint64_t,size_t>> live;
        for(const auto &request:requests) for(size_t i=0;i<request.offerItems.size();++i) {
            if(!request.offerSelection.empty()&&request.offerSelection[i]!='-')continue;
            const auto &item=request.offerItems[i];live.insert({request.id,i});checked.try_emplace({request.id,i},true);
            if(std::find(senders.begin(),senders.end(),request.peer)==senders.end())senders.push_back(request.peer);
            leaves.push_back({false,request.peer,item.name,request.id,item.total,i,static_cast<size_t>(item.count),item.folder});
        }
        std::erase_if(checked,[&](const auto &entry){return !live.contains(entry.first);});rows.clear();
        for(const auto &sender:senders) {
            ApprovalRow group;group.sender=true;group.peer=group.name=sender;
            for(const auto &leaf:leaves)if(leaf.peer==sender){++group.count;group.total+=leaf.total;}
            rows.push_back(group);if(!collapsed.contains(sender))for(const auto &leaf:leaves)if(leaf.peer==sender)rows.push_back(leaf);
        }
    }
    bool Checked(const ApprovalRow &row)const {if(!row.sender){const auto it=checked.find({row.request,row.item});return it!=checked.end()&&it->second;}for(const auto &leaf:leaves)if(leaf.peer==row.peer&&!Checked(leaf))return false;return true;}
    size_t Selected()const {return std::count_if(checked.begin(),checked.end(),[](const auto &entry){return entry.second;});}
    bool All()const {return !checked.empty()&&Selected()==checked.size();}
    void ToggleAll(){const bool value=!All();for(auto &[key,selected]:checked)selected=value;}
    void Toggle(const ApprovalRow &row){const bool value=!Checked(row);if(row.sender){for(const auto &leaf:leaves)if(leaf.peer==row.peer)checked[{leaf.request,leaf.item}]=value;}else checked[{row.request,row.item}]=value;}
    std::map<uint64_t,std::string> Decisions(const std::vector<Transfer> &requests,bool accept)const {
        std::map<uint64_t,std::string> result;
        for(const auto &request:requests){std::string mask(request.offerItems.size(),'-');for(size_t i=0;i<mask.size();++i){const auto it=checked.find({request.id,i});if(it!=checked.end()&&it->second)mask[i]=accept?'1':'0';}if(mask.find_first_of("01")!=std::string::npos)result[request.id]=mask;}
        return result;
    }
};
}
