#include "features/project_order.h"
#include "data/data_store.h"
#include "ui/project_list_view.h"
#include "ui/drawing_utils.h"
#include <windowsx.h>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
using namespace aicli::ui;
struct DrawnRow { UINT row{}, flags{}; RECT rect{}; };
std::vector<DrawnRow> drawn;
int selection_events{}, commits{}, committed_source{-1}, committed_destination{-1};
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
LRESULT CALLBACK OwnerProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_COMMAND && HIWORD(wparam) == LBN_SELCHANGE) { ++selection_events; return 0; }
    if (message == WM_DRAWITEM) {
        const auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
        drawn.push_back({item.itemID, item.itemState, item.rcItem});
        HBRUSH brush = CreateSolidBrush((item.itemState & kProjectItemDragPreview) ? RGB(30,80,120) : RGB(30,40,50));
        FillRect(item.hDC, &item.rcItem, brush);
        DeleteObject(brush);
        return TRUE;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
void Pump(DWORD milliseconds = 0) {
    const auto until = GetTickCount64() + milliseconds;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        if (GetTickCount64() < until) MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
    } while (GetTickCount64() < until);
}
void Mouse(HWND list, UINT message, int x, int y) {
    SendMessageW(list, message, message == WM_LBUTTONUP ? 0 : MK_LBUTTON, MAKELPARAM(x,y));
}
int RowTop(HWND list, int row) {
    RECT rect{};
    Require(SendMessageW(list, LB_GETITEMRECT, row, reinterpret_cast<LPARAM>(&rect)) != LB_ERR, "read row bounds");
    return rect.top;
}
std::vector<ProjectListEntry> Entries(int count) {
    std::vector<ProjectListEntry> entries;
    for (int row = 0; row < count; ++row) entries.push_back({L"Project " + std::to_wstring(row), row});
    return entries;
}
void TestView(HWND owner) {
    Require(RegisterProjectListViewClass(GetModuleHandleW(nullptr)), "register custom list");
    HWND list = CreateProjectListView(owner, 1001, GetModuleHandleW(nullptr));
    Require(list != nullptr, "create isolated list");
    SetWindowPos(list, nullptr, 0, 0, 280, 160, SWP_NOACTIVATE);
    SendMessageW(list, LB_SETITEMHEIGHT, 0, 40);
    ReplaceProjectListEntries(list, Entries(30), 0);
    int range_first = 0, range_last = 29;
    bool enabled = true;
    SetProjectListReorderCallbacks(list, {
        [&](int, int& first, int& last) { first=range_first;last=range_last;return enabled; },
        [&](int source, int destination) {
            ++commits;committed_source=source;committed_destination=destination;
            const int selected=static_cast<int>(SendMessageW(list,LB_GETCURSEL,0,0));
            const auto selected_data=SendMessageW(list,LB_GETITEMDATA,selected,0);
            std::vector<ProjectListEntry> entries;
            for(int row=0;row<SendMessageW(list,LB_GETCOUNT,0,0);++row) {
                const auto data=SendMessageW(list,LB_GETITEMDATA,row,0);
                entries.push_back({L"Project "+std::to_wstring(data),data});
            }
            const auto moving=entries[source];
            entries.erase(entries.begin()+source);entries.insert(entries.begin()+destination,moving);
            const auto next=std::find_if(entries.begin(),entries.end(),[&](const auto& entry){return entry.data==selected_data;});
            const int next_selected=static_cast<int>(next-entries.begin());
            ReplaceProjectListEntries(list,std::move(entries),next_selected);
        }
    });
    // Handle clicks and small movements never switch the active project.
    Mouse(list, WM_LBUTTONDOWN, 15, 60);
    Mouse(list, WM_MOUSEMOVE, 15, 61);
    Mouse(list, WM_LBUTTONUP, 15, 61);
    Require(commits == 0 && selection_events == 0 && SendMessageW(list,LB_GETCURSEL,0,0) == 0,
            "clicking the drag handle must not change the selected terminal");
    Mouse(list,WM_LBUTTONDOWN,15,60);
    SendMessageW(list,WM_KEYDOWN,VK_ESCAPE,0);
    Require(!IsProjectListReordering(list)&&GetCapture()!=list&&commits==0,
            "Escape releases a pending handle gesture before crossing the drag threshold");

    Mouse(list, WM_LBUTTONDOWN, 15, 60);
    Mouse(list, WM_MOUSEMOVE, 90, 112);
    Pump(180);
    drawn.clear();
    RedrawWindow(list,nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW);
    const auto floating = std::find_if(drawn.begin(),drawn.end(),[](const auto& row){return (row.flags&kProjectItemDragPreview)!=0;});
    Require(floating!=drawn.end() && floating->row==1 && floating->rect.left==0 && floating->rect.right==280,
            "floating project uses the original owner renderer and stays horizontally aligned");
    Require(std::count_if(drawn.begin(),drawn.end(),[](const auto& row){return row.row==1;})==1,
            "dragged project is painted once, with no duplicate row content");
    Require(std::any_of(drawn.begin(),drawn.end(),[](const auto& row){return row.row==2 && row.rect.top==40;}),
            "neighboring project animates into the vacated row");
    Mouse(list,WM_LBUTTONUP,90,112);
    Require(commits==0, "drop finishes its landing animation before persistence");
    Pump(180);
    Require(commits==1 && committed_source==1 && committed_destination==2 && !IsProjectListReordering(list),
            "one completed drop produces exactly one reorder");
    Require(selection_events==0 && SendMessageW(list,LB_GETCURSEL,0,0)==0, "drag never activates another terminal");

    // Outside release, Escape, capture loss and model refresh all cancel.
    const auto cancel_case = [&](int mode) {
        Mouse(list,WM_LBUTTONDOWN,15,60); Mouse(list,WM_MOUSEMOVE,15,110);
        if (mode==0) Mouse(list,WM_LBUTTONUP,300,110);
        if (mode==1) SendMessageW(list,WM_KEYDOWN,VK_ESCAPE,0);
        if (mode==2) ReleaseCapture();
        if (mode==3) ReplaceProjectListEntries(list,Entries(30),0);
        if (mode==4) SendMessageW(list,WM_CANCELMODE,0,0);
        Pump(180);
        Require(commits==1&&!IsProjectListReordering(list),"cancelled drag must never save a changed order");
    };
    for (int mode=0;mode<5;++mode) cancel_case(mode);

    // Content-relative hit testing and stationary edge scroll on a long list.
    SendMessageW(list,LB_SETTOPINDEX,10,0);
    Mouse(list,WM_LBUTTONDOWN,15,60); // row 11
    Mouse(list,WM_MOUSEMOVE,15,5);
    Mouse(list,WM_LBUTTONUP,15,5);
    Pump(180);
    Require(committed_source==11&&committed_destination==10,"viewport top must not mean first project in the entire list");
    Mouse(list,WM_LBUTTONDOWN,15,60);
    Mouse(list,WM_MOUSEMOVE,15,159);
    const int before=RowTop(list,10);
    bool pixel_step=false;
    int previous_top=before;
    for(int step=0;step<50;++step) {
        Pump(5);
        const int top=RowTop(list,10);
        if(top!=previous_top&&std::abs(top-previous_top)<40)pixel_step=true;
        previous_top=top;
    }
    Require(RowTop(list,10)<before,"holding the pointer still near the edge continues pixel scrolling");
    const int offset_after_scroll=RowTop(list,10);
    CancelProjectListReorder(list);
    ReplaceProjectListEntries(list,Entries(30),0);
    Require(RowTop(list,10)==offset_after_scroll&&selection_events==0,
            "atomic reorder replacement retains exact pixel viewport without selection callbacks");
    Require(pixel_step,"edge scrolling advances in pixels, not whole rows");

    SendMessageW(list,LB_SETTOPINDEX,0,0);
    range_first=0;range_last=2;
    const int before_group=commits;
    Mouse(list,WM_LBUTTONDOWN,15,60); Mouse(list,WM_MOUSEMOVE,15,155); Mouse(list,WM_LBUTTONUP,15,155);
    Pump(180);
    Require(commits==before_group,"dropping in another group cannot commit a misleading position");
    enabled=false;
    Mouse(list,WM_LBUTTONDOWN,15,60); Mouse(list,WM_MOUSEMOVE,15,110); Mouse(list,WM_LBUTTONUP,15,110);
    Require(!IsProjectListReordering(list)&&commits==before_group,"disabled/search view cannot reorder");
    Mouse(list,WM_LBUTTONDOWN,120,20);
    Mouse(list,WM_LBUTTONUP,120,20);
    Require(SendMessageW(list,LB_GETCURSEL,0,0)==0,"normal row clicks still select projects");

    enabled=true;range_first=0;range_last=29;
    const int before_noop=commits;
    Mouse(list,WM_LBUTTONDOWN,15,60);Mouse(list,WM_MOUSEMOVE,15,66);Mouse(list,WM_LBUTTONUP,15,66);
    Pump(180);
    Require(commits==before_noop,"dropping in the same position does not write order");
    Mouse(list,WM_LBUTTONDOWN,15,60);Mouse(list,WM_MOUSEMOVE,15,85);Mouse(list,WM_MOUSEMOVE,15,79);
    Pump(180);
    drawn.clear();RedrawWindow(list,nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW);
    Require(std::any_of(drawn.begin(),drawn.end(),[](const auto& row){return row.row==2&&row.rect.top==40;}),
            "small pointer jitter at a boundary does not reverse the insertion position");
    CancelProjectListReorder(list);

    ReplaceProjectListEntries(list,Entries(2),0);range_last=1;
    Mouse(list,WM_LBUTTONDOWN,15,20);Mouse(list,WM_MOUSEMOVE,15,130);Mouse(list,WM_LBUTTONUP,15,130);
    Pump(180);
    Require(committed_source==0&&committed_destination==1&&SendMessageW(list,LB_GETCURSEL,0,0)==1,
            "empty space below a short list accepts insertion at the end and retains selected project identity");
    DestroyWindow(list);
    std::cout << "Native reorder input, single row rendering, animation, cancellation, pixel scrolling and selection preservation passed\n";
}

void TestOrder(const std::filesystem::path& root) {
    aicli::DataStore store;
    Require(store.Initialize((root/L"ordering.db").wstring()),"create temporary ordering database");
    std::vector<aicli::Project> projects;
    const auto type=store.LoadProjectTypes().front().name;
    const auto group=store.LoadGroups().front().name;
    for (int index=0;index<6;++index) {
        aicli::Project project;
        project.name=L"Project "+std::to_wstring(index);project.type=type;project.group=group;
        project.folder=root.wstring();project.command=L"echo test";
        Require(store.SaveProject(project),"seed group project");projects.push_back(project);
    }
    projects=store.LoadProjects();
    const std::vector<std::wstring> visible{projects[0].id,projects[2].id,projects[4].id};
    const auto plan=aicli::project_order::MoveVisible(projects,visible,0,2,group);
    Require(plan&&plan->ids==std::vector<std::wstring>{projects[2].id,projects[1].id,projects[4].id,projects[3].id,projects[0].id,projects[5].id},
            "started-only reorder preserves hidden projects in their original slots");
    Require(store.ReorderProjects(plan->group,plan->ids),"commit complete group order atomically");
    store.Close();Require(store.Initialize((root/L"ordering.db").wstring()),"reopen reordered database");
    const auto persisted=store.LoadProjects();
    for(size_t row=0;row<plan->ids.size();++row)Require(persisted[row].id==plan->ids[row],"persisted order matches visual plan");
    auto other=projects.back();other.id=L"other";other.group=L"other group";projects.push_back(other);
    Require(!aicli::project_order::MoveVisible(projects,{projects[0].id,other.id},0,1,group),"cross-group reorder rejected");
    Require(!aicli::project_order::MoveVisible(projects,{projects[0].id,L"deleted"},0,1,group),"stale/deleted project rejected");
    Require(!aicli::project_order::MoveVisible(projects,{projects[0].id,projects[0].id},0,1,group),"duplicate project rejected");
    Require(!aicli::project_order::MoveVisible(projects,visible,0,0,group),"unchanged order produces no database write");
    std::cout << "Complete group ordering, hidden project preservation and SQLite restart persistence passed\n";
}
}
int main() {
    HWND owner{};
    std::filesystem::path root;
    try {
        GdiplusRuntime drawing;
        auto temp=std::filesystem::absolute(std::filesystem::temp_directory_path()).lexically_normal();
        if(temp.filename().empty())temp=temp.parent_path();
        root=temp/(L"aicli-reorder-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(root);
        TestOrder(root);
        WNDCLASSW type{};type.lpfnWndProc=OwnerProc;type.hInstance=GetModuleHandleW(nullptr);type.lpszClassName=L"AiCliReorderTestOwner";
        RegisterClassW(&type);
        owner=CreateWindowW(type.lpszClassName,L"",WS_POPUP|WS_VISIBLE,-30000,-30000,300,200,nullptr,nullptr,type.hInstance,nullptr);
        Require(owner!=nullptr,"create isolated native owner");
        TestView(owner);
        DestroyWindow(owner);owner=nullptr;
        Require(root.parent_path()==temp,"test artifacts remain in temporary directory");
        std::filesystem::remove_all(root);
        return 0;
    } catch(const std::exception& error) {
        if(owner)DestroyWindow(owner);
        std::cerr<<error.what()<<'\n';return 1;
    }
}
