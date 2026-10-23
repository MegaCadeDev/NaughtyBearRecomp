#include <stdafx.h>
#include "launcher.h"
#include <mods/mods.h>
#include <os/logger.h>
#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>

// A small window of its own, drawn by Dear ImGui with SDL's 2D renderer, before the game's
// window and Vulkan renderer exist. Works with the mouse, the keyboard and controllers.
namespace launcher
{
    namespace
    {
        const char* CharacterLabel(const std::string& package)
        {
            if (package.empty())
                return "Off";
            for (const mods::Character& character : mods::Characters())
                if (character.package == package)
                    return character.label.c_str();
            return package.c_str();
        }
    }

    bool Run(const std::filesystem::path& modsDirectory, bool force)
    {
        std::vector<mods::ModInfo> list = mods::List(modsDirectory);
        if (list.empty())
            return true;
        bool showMenu = mods::ShowMenu(modsDirectory);
        if (!showMenu && !force)
            return true;

        SDL_Window* window = SDL_CreateWindow("Naughty Bear - Mods", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 900, 540,
            SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
        SDL_Renderer* renderer = window != nullptr ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
        if (renderer == nullptr && window != nullptr)
            renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
        if (renderer == nullptr)
        {
            // No menu: start with the mods as they're set.
            LOGFN_WARNING("Mods menu: could not open its window ({})", SDL_GetError());
            if (window != nullptr)
                SDL_DestroyWindow(window);
            return true;
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        io.Fonts->AddFontDefaultVector();
        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();
        style.FontSizeBase = 20.0f;
        style.ScaleAllSizes(1.25f);
        style.WindowPadding = ImVec2(24.0f, 20.0f);
        ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
        ImGui_ImplSDLRenderer2_Init(renderer);

        bool play = false, done = false, firstFrame = true;
        while (!done)
        {
            SDL_Event event;
            while (SDL_PollEvent(&event))
            {
                ImGui_ImplSDL2_ProcessEvent(&event);
                if (event.type == SDL_QUIT || (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE))
                    done = true;
                // The Guide button closes the game, as it does in game.
                if (event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE)
                    done = true;
            }

            ImGui_ImplSDLRenderer2_NewFrame();
            ImGui_ImplSDL2_NewFrame();
            ImGui::NewFrame();

            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::Begin("Mods", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

            ImGui::PushFont(nullptr, style.FontSizeBase * 1.6f);
            ImGui::TextUnformatted("Mods");
            ImGui::PopFont();
            ImGui::TextDisabled("Mods are made by players. They aren't part of the game or this project.");
            ImGui::TextDisabled("Choose who each mod replaces; each character can have one mod.");
            ImGui::Spacing();

            float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f + style.ItemSpacing.y * 4.0f;
            if (ImGui::BeginTable("mods", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_PadOuterX,
                ImVec2(0.0f, -footer)))
            {
                ImGui::TableSetupColumn("Mod", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Replaces", ImGuiTableColumnFlags_WidthStretch, 1.1f);
                ImGui::TableSetupColumn("Credits", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();
                for (size_t i = 0; i < list.size(); i++)
                {
                    mods::ModInfo& mod = list[i];
                    ImGui::PushID(int(i));
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(mod.name.c_str());
                    if (mod.name != mod.id)
                        ImGui::SetItemTooltip("%s", mod.id.c_str());

                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (mod.isModel)
                    {
                        // One mod per character: choosing a character another mod has turns
                        // that mod off.
                        if (ImGui::BeginCombo("##character", CharacterLabel(mod.character), ImGuiComboFlags_HeightLarge))
                        {
                            if (ImGui::Selectable("Off", mod.character.empty()))
                                mod.character.clear();
                            for (const mods::Character& character : mods::Characters())
                            {
                                const mods::ModInfo* holder = nullptr;
                                for (const mods::ModInfo& other : list)
                                    if (&other != &mod && other.character == character.package)
                                        holder = &other;
                                std::string label = holder != nullptr ? fmt::format("{}  (now: {})", character.label, holder->name) : character.label;
                                if (ImGui::Selectable(label.c_str(), mod.character == character.package))
                                {
                                    for (mods::ModInfo& other : list)
                                        if (other.character == character.package)
                                            other.character.clear();
                                    mod.character = character.package;
                                }
                            }
                            ImGui::EndCombo();
                        }
                    }
                    else
                    {
                        std::string on = fmt::format("On ({})", mod.meshes);
                        if (ImGui::BeginCombo("##enabled", mod.enabled ? on.c_str() : "Off"))
                        {
                            if (ImGui::Selectable("Off", !mod.enabled))
                                mod.enabled = false;
                            if (ImGui::Selectable(on.c_str(), mod.enabled))
                                mod.enabled = true;
                            ImGui::EndCombo();
                        }
                    }

                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextWrapped("%s", mod.credits.empty() ? "-" : mod.credits.c_str());
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }

            ImGui::Spacing();
            ImGui::Checkbox("Show this menu when the game starts", &showMenu);
            if (!showMenu)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(start with --mods-menu to see it again)");
            }
            ImGui::Spacing();
            ImVec2 button(160.0f, 0.0f);
            if (ImGui::Button("Play", button))
                play = done = true;
            if (firstFrame)
                ImGui::SetKeyboardFocusHere(-1);
            ImGui::SameLine();
            if (ImGui::Button("Quit", button))
                done = true;
            ImGui::End();

            ImGui::Render();
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
            SDL_RenderPresent(renderer);
            firstFrame = false;
        }

        mods::SaveSettings(modsDirectory, list, showMenu);
        ImGui_ImplSDLRenderer2_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        return play;
    }
}
