package com.aitest.aicli;

import android.graphics.Color;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.recyclerview.widget.RecyclerView;

import java.util.ArrayList;
import java.util.List;

final class ConversationAdapter extends RecyclerView.Adapter<ConversationAdapter.Holder> {
    interface OnConversationClickListener {
        void onConversationClicked(AicliClient.Conversation conversation);
    }

    private final List<AicliClient.Conversation> items = new ArrayList<>();
    private final OnConversationClickListener listener;

    ConversationAdapter(OnConversationClickListener listener) {
        this.listener = listener;
    }

    void replace(List<AicliClient.Conversation> conversations) {
        items.clear();
        items.addAll(conversations);
        notifyDataSetChanged();
    }

    @NonNull
    @Override
    public Holder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
        LinearLayout row = new LinearLayout(parent.getContext());
        row.setOrientation(LinearLayout.VERTICAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        int padding = (int) (12 * parent.getResources().getDisplayMetrics().density);
        row.setPadding(padding, padding / 2, padding, padding / 2);
        TextView title = new TextView(parent.getContext());
        title.setTextColor(Color.DKGRAY);
        title.setTextSize(16);
        TextView status = new TextView(parent.getContext());
        status.setTextColor(Color.GRAY);
        status.setTextSize(12);
        row.addView(title);
        row.addView(status);
        return new Holder(row, title, status);
    }

    @Override
    public void onBindViewHolder(@NonNull Holder holder, int position) {
        AicliClient.Conversation item = items.get(position);
        holder.title.setText(item.name);
        holder.status.setText(item.status);
        holder.itemView.setOnClickListener(view -> listener.onConversationClicked(item));
    }

    @Override
    public int getItemCount() {
        return items.size();
    }

    static final class Holder extends RecyclerView.ViewHolder {
        final TextView title;
        final TextView status;

        Holder(@NonNull View itemView, TextView title, TextView status) {
            super(itemView);
            this.title = title;
            this.status = status;
        }
    }
}
