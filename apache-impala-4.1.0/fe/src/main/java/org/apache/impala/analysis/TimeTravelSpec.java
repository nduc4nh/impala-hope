// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package org.apache.impala.analysis;

import static org.apache.impala.analysis.ToSqlOptions.DEFAULT;

import org.apache.impala.catalog.Type;
import org.apache.impala.common.AnalysisException;
import org.apache.impala.common.InternalException;
import org.apache.impala.util.ExprUtil;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import com.google.common.base.Preconditions;

// Contains information from the 'FOR SYSTEM_TIME AS OF', or 'FOR SYSTEM_VERSION AS OF'
// clauses. Based on that information we can support time travel with table formats
// that support it, e.g. Iceberg.
// TODO(IMPALA-9773): Kudu
public class TimeTravelSpec extends StmtNode {
  private final static Logger LOG = LoggerFactory.getLogger(TimeTravelSpec.class);

  public enum Kind {
    TIME_AS_OF,
    VERSION_AS_OF
  }

  // Time travel can be time-based or version-based.
  private Kind kind_;

  // Expression used in the 'FOR SYSTEM_* AS OF' clause.
  private Expr asOfExpr_;

  // For Iceberg tables this is the snapshot id.
  private long asOfVersion_ = -1;

  // Iceberg uses millis, Kudu uses micros for time travel, so using micros here.
  private long asOfMicros_ = -1;

  public Kind getKind() { return kind_; }

  public long getAsOfVersion() { return asOfVersion_; }

  public long getAsOfMillis() { return asOfMicros_ == -1 ? -1 : asOfMicros_ / 1000; }

  public long getAsOfMicros() { return asOfMicros_; }

  public TimeTravelSpec(Kind kind, Expr asOfExpr) {
    Preconditions.checkNotNull(asOfExpr);
    kind_ = kind;
    asOfExpr_ = asOfExpr;
  }

  protected TimeTravelSpec(TimeTravelSpec other) {
    kind_ = other.kind_;
    asOfExpr_ = other.asOfExpr_.clone();
    asOfVersion_ = other.asOfVersion_;
    asOfMicros_ = other.asOfMicros_;
  }

  @Override
  public TimeTravelSpec clone() { return new TimeTravelSpec(this); }

  @Override
  public void analyze(Analyzer analyzer) throws AnalysisException {
    switch (kind_) {
      case TIME_AS_OF: analyzeTimeBased(analyzer); break;
      case VERSION_AS_OF: analyzeVersionBased(analyzer); break;
    }
  }

  private void analyzeTimeBased(Analyzer analyzer) throws AnalysisException {
    Preconditions.checkNotNull(asOfExpr_);
    asOfExpr_.analyze(analyzer);
    if (!asOfExpr_.isConstant()) {
      throw new AnalysisException(
          "FOR SYSTEM_TIME AS OF <expression> must be a constant expression: " + toSql());
    }
    if (asOfExpr_.getType().isStringType()) {
      asOfExpr_ = new CastExpr(Type.TIMESTAMP, asOfExpr_);
    }
    if (!asOfExpr_.getType().isTimestamp()) {
      throw new AnalysisException(
          "FOR SYSTEM_TIME AS OF <expression> must be a timestamp type but is '" +
              asOfExpr_.getType() + "': " + asOfExpr_.toSql());
    }
    try {
      asOfMicros_ = ExprUtil.localTimestampToUnixTimeMicros(analyzer, asOfExpr_);
      LOG.debug("FOR SYSTEM_TIME AS OF micros: " + String.valueOf(asOfMicros_));
    } catch (InternalException ie) {
      throw new AnalysisException(
          "Invalid TIMESTAMP expression: " + ie.getMessage(), ie);
    }
  }

  private void analyzeVersionBased(Analyzer analyzer) throws AnalysisException {
    Preconditions.checkNotNull(asOfExpr_);
    asOfExpr_.analyze(analyzer);
    if (!(asOfExpr_ instanceof LiteralExpr)) {
      throw new AnalysisException(
          "FOR SYSTEM_VERSION AS OF <expression> must be an integer literal: "
          + toSql());
    }
    if (!asOfExpr_.getType().isIntegerType()) {
      throw new AnalysisException(
          "FOR SYSTEM_VERSION AS OF <expression> must be an integer type but is '" +
              asOfExpr_.getType() + "': " + asOfExpr_.toSql());
    }
    asOfVersion_ = asOfExpr_.evalToInteger(analyzer, "SYSTEM_VERSION AS OF");
    if (asOfVersion_ < 0) {
      throw new AnalysisException(
          "Invalid version number has been given to SYSTEM_VERSION AS OF: " +
          String.valueOf(asOfVersion_));
    }
    LOG.debug("FOR SYSTEM_VERSION AS OF version: " + String.valueOf(asOfVersion_));
  }

  public void reset() {
    asOfVersion_ = -1;
    asOfMicros_ = -1;
  }

  @Override
  public String toSql(ToSqlOptions options) {
    return String.format("FOR %s AS OF %s",
        kind_ == Kind.TIME_AS_OF ? "SYSTEM_TIME" : "SYSTEM_VERSION",
        asOfExpr_.toSql());
  }

  @Override
  public final String toSql() {
    return toSql(DEFAULT);
  }
}
