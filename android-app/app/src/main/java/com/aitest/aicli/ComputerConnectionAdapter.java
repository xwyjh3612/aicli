package com.aitest.aicli;

import android.graphics.drawable.GradientDrawable;
import android.view.animation.AlphaAnimation;
import android.view.animation.Animation;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.core.content.ContextCompat;
import androidx.recyclerview.widget.RecyclerView;

import com.google.android.material.card.MaterialCardView;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** Renders the Android-side list of authorized desktop services. */
final class ComputerConnectionAdapter
        extends RecyclerView.Adapter<ComputerConnectionAdapter.Holder> {
    enum Health {
        NOT_CONNECTED,
        CONNECTING,
        CONNECTED,
        FAILED,
    }

    interface Listener {
        void onSelect(ComputerConnectionStore.Computer computer);
        void onManage(ComputerConnectionStore.Computer computer, View anchor);
    }

    private final List<ComputerConnectionStore.Computer> items = new ArrayList<>();
    private final Map<String, Health> health = new HashMap<>();
    private final Listener listener;
    private String activeComputerId = "";

    ComputerConnectionAdapter(Listener listener) {
        this.listener = listener;
        setHasStableIds(true);
    }

    void replace(List<ComputerConnectionStore.Computer> computers, String activeId) {
        items.clear();
        items.addAll(computers);
        activeComputerId = activeId == null ? "" : activeId;
        health.keySet().removeIf(id -> computers.stream()
                .noneMatch(computer -> computer.computerId.equals(id)));
        for (ComputerConnectionStore.Computer computer : computers) {
            if (!health.containsKey(computer.computerId)) {
                health.put(computer.computerId, Health.NOT_CONNECTED);
            }
        }
        notifyDataSetChanged();
    }

    void setHealth(String computerId, Health value) {
        if (computerId == null || computerId.isEmpty() || value == null) return;
        health.put(computerId, value);
        for (int index = 0; index < items.size(); index++) {
            if (!computerId.equals(items.get(index).computerId)) continue;
            notifyItemChanged(index, "health");
            return;
        }
    }

    @Override public long getItemId(int position) {
        return items.get(position).computerId.hashCode();
    }

    @NonNull
    @Override public Holder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
        return new Holder(LayoutInflater.from(parent.getContext())
                .inflate(R.layout.item_computer_connection, parent, false));
    }

    @Override public void onBindViewHolder(@NonNull Holder holder, int position) {
        ComputerConnectionStore.Computer computer = items.get(position);
        boolean active = computer.computerId.equals(activeComputerId);
        holder.title.setText(computer.displayName());
        holder.summary.setText(summary(computer));
        holder.badge.setVisibility(active ? View.VISIBLE : View.GONE);

        Health currentHealth = health.getOrDefault(
                computer.computerId, Health.NOT_CONNECTED);
        holder.statusDot.clearAnimation();
        int dotColor;
        int textColor;
        String statusText;
        if (currentHealth == Health.CONNECTED) {
            dotColor = ContextCompat.getColor(holder.itemView.getContext(), R.color.aicli_running);
            textColor = dotColor;
            statusText = "已连接";
        } else if (currentHealth == Health.FAILED) {
            dotColor = ContextCompat.getColor(holder.itemView.getContext(),
                    R.color.aicli_connection_failed);
            textColor = dotColor;
            statusText = "连接失败";
        } else if (currentHealth == Health.CONNECTING) {
            dotColor = ContextCompat.getColor(holder.itemView.getContext(),
                    R.color.aicli_connection_connecting);
            textColor = dotColor;
            statusText = "连接中";
            AlphaAnimation pulse = new AlphaAnimation(0.28f, 1f);
            pulse.setDuration(520L);
            pulse.setRepeatMode(Animation.REVERSE);
            pulse.setRepeatCount(Animation.INFINITE);
            holder.statusDot.startAnimation(pulse);
        } else {
            dotColor = ContextCompat.getColor(holder.itemView.getContext(),
                    R.color.aicli_connection_offline);
            textColor = ContextCompat.getColor(holder.itemView.getContext(),
                    R.color.aicli_text_tertiary);
            statusText = "未连接";
        }
        GradientDrawable dot = new GradientDrawable();
        dot.setShape(GradientDrawable.OVAL);
        dot.setColor(dotColor);
        holder.statusDot.setBackground(dot);
        holder.onlineStatus.setText(statusText);
        holder.onlineStatus.setTextColor(textColor);

        holder.card.setCardBackgroundColor(ContextCompat.getColor(holder.itemView.getContext(),
                active ? R.color.aicli_surface_selected : R.color.aicli_surface_high));
        holder.card.setStrokeColor(ContextCompat.getColor(holder.itemView.getContext(),
                active ? R.color.aicli_primary : R.color.aicli_outline));
        holder.itemView.setContentDescription((active ? "当前服务端，" : "切换到服务端，")
                + computer.displayName() + "，" + statusText);
        holder.itemView.setOnClickListener(view -> listener.onSelect(computer));
        holder.manage.setContentDescription("管理 " + computer.displayName());
        holder.manage.setOnClickListener(view -> listener.onManage(computer, view));
        holder.itemView.setOnLongClickListener(view -> {
            listener.onManage(computer, holder.manage);
            return true;
        });
    }

    @Override public int getItemCount() { return items.size(); }

    private static String summary(ComputerConnectionStore.Computer computer) {
        StringBuilder value = new StringBuilder();
        if (!computer.remark.isEmpty() && !computer.name.isEmpty()) {
            value.append(computer.name);
        }
        String routes = !computer.localUrl.isEmpty() && !computer.remoteUrl.isEmpty()
                ? "局域网 + 公网"
                : !computer.localUrl.isEmpty() ? "仅局域网"
                : !computer.remoteUrl.isEmpty() ? "仅公网" : "";
        if (!routes.isEmpty()) {
            if (value.length() > 0) value.append("  ·  ");
            value.append(routes);
            if (!computer.localUrl.isEmpty() && !computer.remoteUrl.isEmpty()) {
                value.append("  ·  ").append(computer.remotePreferred()
                        ? "广域网优先" : "局域网优先");
            }
        }
        if (value.length() == 0) value.append(computer.computerId);
        return value.toString();
    }

    static final class Holder extends RecyclerView.ViewHolder {
        final MaterialCardView card;
        final View statusDot;
        final TextView title;
        final TextView summary;
        final TextView onlineStatus;
        final TextView badge;
        final TextView manage;

        Holder(@NonNull View itemView) {
            super(itemView);
            card = itemView.findViewById(R.id.computerCard);
            statusDot = itemView.findViewById(R.id.computerStatusDot);
            title = itemView.findViewById(R.id.computerName);
            summary = itemView.findViewById(R.id.computerSummary);
            onlineStatus = itemView.findViewById(R.id.computerOnlineStatus);
            badge = itemView.findViewById(R.id.computerActiveBadge);
            manage = itemView.findViewById(R.id.computerManageButton);
        }
    }
}
