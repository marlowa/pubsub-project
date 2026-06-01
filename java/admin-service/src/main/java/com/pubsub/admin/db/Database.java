package com.pubsub.admin.db;

import com.pubsub.admin.Config;
import com.zaxxer.hikari.HikariConfig;
import com.zaxxer.hikari.HikariDataSource;

import javax.sql.DataSource;

public class Database {
    private Database() {}

    public static DataSource createDataSource(Config config) {
        HikariConfig hikari = new HikariConfig();
        hikari.setJdbcUrl(config.dbUrl());
        hikari.setUsername(config.dbUsername());
        hikari.setPassword(config.dbPassword());
        hikari.setMaximumPoolSize(10);
        hikari.setMinimumIdle(2);
        return new HikariDataSource(hikari);
    }
}
