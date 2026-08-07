package com.aitest.aicli;

import android.graphics.drawable.GradientDrawable;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;

import androidx.annotation.ColorRes;
import androidx.annotation.NonNull;
import androidx.core.content.ContextCompat;
import androidx.recyclerview.widget.RecyclerView;

import com.google.android.material.card.MaterialCardView;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

final class ConversationAdapter extends RecyclerView.Adapter<ConversationAdapter.Holder> {
    interface OnConversationClickListener {
        void onConversationClicked(AicliClient.Conversation conversation);
    }

    private final List<AicliClient.Conversation> items = new ArrayList<>();
    private final OnConversationClickListener listener;
    private String selectedId = "";

    ConversationAdapter(OnConversationClickListener listener) {
        this.listener = listener;
        setHasStableIds(true);
    }

    void replace(List<AicliClient.Conversation> conversations) {
        items.clear();
        items.addAll(conversations);
        notifyDataSetChanged();
    }

    void select(String conversationId) {
        selectedId = conversationId == null ? "" : conversationId;
        notifyDataSetChanged();
    }

    @Override
    public long getItemId(int position) {
        return items.get(position).id.hashCode();
    }

    @NonNull
    @Override
    public Holder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
        View item = LayoutInflater.from(parent.getContext())
                .inflate(R.layout.item_conversation, parent, false);
        return new Holder(item);
    }

    @Override
    public void onBindViewHolder(@NonNull Holder holder, int position) {
        AicliClient.Conversation item = items.get(position);
        StatusVisual visual = visualFor(item.status);
        holder.title.setText(item.name);
        String type = item.type.isEmpty() ? item.provider : item.type;
        String group = item.group.isEmpty() ? "默认分组" : item.group;
        holder.summary.setText(visual.label + "  ·  " + type + "  ·  " + group);

        int dotColor = ContextCompat.getColor(holder.itemView.getContext(), visual.color);
        GradientDrawable dot = new GradientDrawable();
        dot.setShape(GradientDrawable.OVAL);
        dot.setColor(dotColor);
        holder.statusDot.setBackground(dot);

        boolean selected = item.id.equals(selectedId);
        holder.card.setCardBackgroundColor(ContextCompat.getColor(holder.itemView.getContext(),
                selected ? R.color.aicli_surface_selected : R.color.aicli_surface));
        holder.card.setStrokeColor(ContextCompat.getColor(holder.itemView.getContext(),
                selected ? R.color.aicli_primary : R.color.aicli_outline));
        holder.card.setStrokeWidth(dp(holder.itemView, selected ? 1.5f : 1f));
        holder.itemView.setOnClickListener(view -> listener.onConversationClicked(item));
    }

    @Override
    public int getItemCount() {
        return items.size();
    }

    static StatusVisual visualFor(String value) {
        String status = value == null ? "" : value.toLowerCase(Locale.ROOT);
        if ("running".equals(status)) return new StatusVisual("运行中", R.color.aicli_running);
        if ("paused".equals(status)) return new StatusVisual("已暂停", R.color.aicli_paused);
        if ("cancelled".equals(status)) return new StatusVisual("已取消", R.color.aicli_stopped);
        if ("error".equals(status) || "failed".equals(status)) {
            return new StatusVisual("运行失败", R.color.aicli_stopped);
        }
        return new StatusVisual("就绪", R.color.aicli_ready);
    }

    private static int dp(View view, float value) {
        return Math.round(value * view.getResources().getDisplayMetrics().density);
    }

    static final class StatusVisual {
        final String label;
        @ColorRes final int color;

        StatusVisual(String label, @ColorRes int color) {
            this.label = label;
            this.color = color;
        }
    }

    static final class Holder extends RecyclerView.ViewHolder {
        final MaterialCardView card;
        final View statusDot;
        final TextView title;
        final TextView summary;

        Holder(@NonNull View itemView) {
            super(itemView);
            card = itemView.findViewById(R.id.conversationCard);
            statusDot = itemView.findViewById(R.id.statusDot);
            title = itemView.findViewById(R.id.conversationName);
            summary = itemView.findViewById(R.id.conversationSummary);
        }
    }
}
