// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

pub mod fetch;
pub mod variable;

use {
    super::config::{self, DataFetcher, DiagnosticData, Source},
    fetch::{InspectFetcher, KeyValueFetcher, SelectorString, SelectorType, TextFetcher},
    fuchsia_inspect_node_hierarchy::{ArrayContent, Property as DiagnosticProperty},
    injectable_time::{FakeTime, TimeSource},
    lazy_static::lazy_static,
    serde::{Deserialize, Deserializer},
    serde_json::Value as JsonValue,
    std::{clone::Clone, cmp::min, collections::HashMap, convert::TryFrom},
    variable::VariableName,
};

/// The contents of a single Metric. Metrics produce a value for use in Actions or other Metrics.
#[derive(Clone, Debug)]
pub enum Metric {
    /// Selector tells where to find a value in the Inspect data.
    // Note: This can't be a fidl_fuchsia_diagnostics::Selector because it's not deserializable or
    // cloneable.
    Selector(SelectorString),
    /// Eval contains an arithmetic expression,
    // TODO(cphoenix): Parse and validate this at load-time.
    Eval(String),
}

impl<'de> Deserialize<'de> for Metric {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        let value = String::deserialize(deserializer)?;
        if SelectorString::is_selector(&value) {
            Ok(Metric::Selector(SelectorString::try_from(value).map_err(serde::de::Error::custom)?))
        } else {
            Ok(Metric::Eval(value))
        }
    }
}

impl std::fmt::Display for Metric {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Metric::Selector(s) => write!(f, "{:?}", s),
            Metric::Eval(s) => write!(f, "{}", s),
        }
    }
}

/// [Metrics] are a map from namespaces to the named [Metric]s stored within that namespace.
pub type Metrics = HashMap<String, HashMap<String, Metric>>;

/// Contains all the information needed to look up and evaluate a Metric - other
/// [Metric]s that may be referred to, and a source of input values to calculate on.
///
/// Note: MetricState uses a single Now() value for all evaluations. If a MetricState is
/// retained and used for multiple evaluations at different times, provide a way to update
/// the `now` field.
pub struct MetricState<'a> {
    pub metrics: &'a Metrics,
    pub fetcher: Fetcher<'a>,
    now: i64,
}

/// [Fetcher] is a source of values to feed into the calculations. It may contain data either
/// from snapshot.zip files (e.g. inspect.json data that can be accessed via "select" entries)
/// or supplied in the specification of a trial.
pub enum Fetcher<'a> {
    FileData(FileDataFetcher<'a>),
    TrialData(TrialDataFetcher<'a>),
}

/// [FileDataFetcher] contains fetchers for data in snapshot.zip files.
#[derive(Clone)]
pub struct FileDataFetcher<'a> {
    pub inspect: &'a InspectFetcher,
    pub syslog: &'a TextFetcher,
    pub klog: &'a TextFetcher,
    pub bootlog: &'a TextFetcher,
    pub annotations: &'a KeyValueFetcher,
}

impl<'a> FileDataFetcher<'a> {
    pub fn new(data: &'a Vec<DiagnosticData>) -> FileDataFetcher<'a> {
        let mut fetcher = FileDataFetcher {
            inspect: InspectFetcher::ref_empty(),
            syslog: TextFetcher::ref_empty(),
            klog: TextFetcher::ref_empty(),
            bootlog: TextFetcher::ref_empty(),
            annotations: KeyValueFetcher::ref_empty(),
        };
        for DiagnosticData { source, data, .. } in data.iter() {
            match source {
                Source::Inspect => {
                    if let DataFetcher::Inspect(data) = data {
                        fetcher.inspect = data;
                    }
                }
                Source::Syslog => {
                    if let DataFetcher::Text(data) = data {
                        fetcher.syslog = data;
                    }
                }
                Source::Klog => {
                    if let DataFetcher::Text(data) = data {
                        fetcher.klog = data;
                    }
                }
                Source::Bootlog => {
                    if let DataFetcher::Text(data) = data {
                        fetcher.bootlog = data;
                    }
                }
                Source::Annotations => {
                    if let DataFetcher::KeyValue(data) = data {
                        fetcher.annotations = data;
                    }
                }
            }
        }
        fetcher
    }

    fn fetch(&self, selector: &SelectorString) -> MetricValue {
        match selector.selector_type {
            // Selectors return a vector. Non-wildcarded Inspect selectors will return a
            // single element, except in the case of multiple components with the same
            // entry in the "moniker" field, where multiple matches are possible.
            SelectorType::Inspect => MetricValue::Vector(self.inspect.fetch(&selector)),
        }
    }

    // Return a vector of errors encountered by contained fetchers.
    pub fn errors(&self) -> Vec<String> {
        self.inspect.component_errors.iter().map(|e| format!("{}", e)).collect()
    }
}

/// [TrialDataFetcher] stores the key-value lookup for metric names whose values are given as
/// part of a trial (under the "test" section of the .triage files).
#[derive(Clone)]
pub struct TrialDataFetcher<'a> {
    values: &'a HashMap<String, JsonValue>,
    klog: &'a TextFetcher,
    syslog: &'a TextFetcher,
    bootlog: &'a TextFetcher,
    annotations: &'a KeyValueFetcher,
}

lazy_static! {
    static ref EMPTY_JSONVALUES: HashMap<String, JsonValue> = HashMap::new();
}

impl<'a> TrialDataFetcher<'a> {
    pub fn new(values: &'a HashMap<String, JsonValue>) -> TrialDataFetcher<'a> {
        TrialDataFetcher {
            values,
            klog: TextFetcher::ref_empty(),
            syslog: TextFetcher::ref_empty(),
            bootlog: TextFetcher::ref_empty(),
            annotations: KeyValueFetcher::ref_empty(),
        }
    }

    pub fn new_empty() -> TrialDataFetcher<'static> {
        TrialDataFetcher {
            values: &EMPTY_JSONVALUES,
            klog: TextFetcher::ref_empty(),
            syslog: TextFetcher::ref_empty(),
            bootlog: TextFetcher::ref_empty(),
            annotations: KeyValueFetcher::ref_empty(),
        }
    }

    pub fn set_syslog(&mut self, fetcher: &'a TextFetcher) {
        self.syslog = fetcher;
    }

    pub fn set_klog(&mut self, fetcher: &'a TextFetcher) {
        self.klog = fetcher;
    }

    pub fn set_bootlog(&mut self, fetcher: &'a TextFetcher) {
        self.bootlog = fetcher;
    }

    pub fn set_annotations(&mut self, fetcher: &'a KeyValueFetcher) {
        self.annotations = fetcher;
    }

    fn fetch(&self, name: &str) -> MetricValue {
        match self.values.get(name) {
            Some(value) => MetricValue::from(value),
            None => MetricValue::Missing(format!("Value {} not overridden in test", name)),
        }
    }

    fn has_entry(&self, name: &str) -> bool {
        self.values.contains_key(name)
    }
}

/// The calculated or selected value of a Metric.
///
/// Missing means that the value could not be calculated; its String tells
/// the reason.
#[derive(Deserialize, Debug, Clone)]
pub enum MetricValue {
    // Ensure every variant of MetricValue is tested in metric_value_traits().
    // TODO(cphoenix): Support u64.
    Int(i64),
    Float(f64),
    String(String),
    Bool(bool),
    Vector(Vec<MetricValue>),
    Bytes(Vec<u8>),
    Missing(String),
    Lambda(Box<Lambda>),
}

impl PartialEq for MetricValue {
    fn eq(&self, other: &Self) -> bool {
        match (self, other) {
            (MetricValue::Int(l), MetricValue::Int(r)) => l == r,
            (MetricValue::Float(l), MetricValue::Float(r)) => l == r,
            (MetricValue::Bytes(l), MetricValue::Bytes(r)) => l == r,
            (MetricValue::Int(l), MetricValue::Float(r)) => *l as f64 == *r,
            (MetricValue::Float(l), MetricValue::Int(r)) => *l == *r as f64,
            (MetricValue::String(l), MetricValue::String(r)) => l == r,
            (MetricValue::Bool(l), MetricValue::Bool(r)) => l == r,
            (MetricValue::Vector(l), MetricValue::Vector(r)) => l == r,
            _ => false,
        }
    }
}

impl Eq for MetricValue {}

impl std::fmt::Display for MetricValue {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match &*self {
            MetricValue::Int(n) => write!(f, "Int({})", n),
            MetricValue::Float(n) => write!(f, "Float({})", n),
            MetricValue::Bool(n) => write!(f, "Bool({})", n),
            MetricValue::String(n) => write!(f, "String({})", n),
            MetricValue::Vector(n) => write!(f, "Vector({:?})", n),
            MetricValue::Bytes(n) => write!(f, "Bytes({:?})", n),
            MetricValue::Missing(n) => write!(f, "Missing({})", n),
            MetricValue::Lambda(n) => write!(f, "Fn({:?})", n),
        }
    }
}

impl Into<MetricValue> for f64 {
    fn into(self) -> MetricValue {
        MetricValue::Float(self)
    }
}

impl Into<MetricValue> for i64 {
    fn into(self) -> MetricValue {
        MetricValue::Int(self)
    }
}

impl From<DiagnosticProperty> for MetricValue {
    fn from(property: DiagnosticProperty) -> Self {
        match property {
            DiagnosticProperty::String(_name, value) => Self::String(value),
            DiagnosticProperty::Bytes(_name, value) => Self::Bytes(value),
            DiagnosticProperty::Int(_name, value) => Self::Int(value),
            DiagnosticProperty::Uint(_name, value) => Self::Int(value as i64),
            DiagnosticProperty::Double(_name, value) => Self::Float(value),
            DiagnosticProperty::Bool(_name, value) => Self::Bool(value),
            // TODO(cphoenix): Figure out what to do about histograms.
            DiagnosticProperty::DoubleArray(_name, ArrayContent::Values(values)) => {
                Self::Vector(map_vec(&values, |value| Self::Float(*value)))
            }
            DiagnosticProperty::IntArray(_name, ArrayContent::Values(values)) => {
                Self::Vector(map_vec(&values, |value| Self::Int(*value)))
            }
            DiagnosticProperty::UintArray(_name, ArrayContent::Values(values)) => {
                Self::Vector(map_vec(&values, |value| Self::Int(*value as i64)))
            }
            DiagnosticProperty::DoubleArray(_name, ArrayContent::Buckets(_))
            | DiagnosticProperty::IntArray(_name, ArrayContent::Buckets(_))
            | DiagnosticProperty::UintArray(_name, ArrayContent::Buckets(_)) => {
                Self::Missing("Histograms aren't supported (yet)".to_string())
            }
        }
    }
}

impl From<JsonValue> for MetricValue {
    fn from(value: JsonValue) -> Self {
        match value {
            JsonValue::String(value) => Self::String(value),
            JsonValue::Bool(value) => Self::Bool(value),
            JsonValue::Number(_) => Self::from(&value),
            JsonValue::Array(values) => {
                Self::Vector(values.into_iter().map(|v| Self::from(v)).collect())
            }
            _ => Self::Missing("Unsupported JSON type".to_owned()),
        }
    }
}

impl From<&JsonValue> for MetricValue {
    fn from(value: &JsonValue) -> Self {
        match value {
            JsonValue::String(value) => Self::String(value.clone()),
            JsonValue::Bool(value) => Self::Bool(*value),
            JsonValue::Number(value) => {
                if value.is_i64() {
                    Self::Int(value.as_i64().unwrap())
                } else if value.is_u64() {
                    Self::Int(value.as_u64().unwrap() as i64)
                } else if value.is_f64() {
                    Self::Float(value.as_f64().unwrap())
                } else {
                    Self::Missing("Unable to convert JSON number".to_owned())
                }
            }
            JsonValue::Array(values) => {
                Self::Vector(values.iter().map(|v| Self::from(v)).collect())
            }
            _ => Self::Missing("Unsupported JSON type".to_owned()),
        }
    }
}

#[derive(Deserialize, Debug, Clone, PartialEq)]
pub enum Function {
    Add,
    Sub,
    Mul,
    FloatDiv,
    IntDiv,
    Greater,
    Less,
    GreaterEq,
    LessEq,
    Equals,
    NotEq,
    Max,
    Min,
    And,
    Or,
    Not,
    KlogHas,
    SyslogHas,
    BootlogHas,
    Missing,
    Annotation,
    Lambda,
    Map,
    Fold,
    Filter,
    Count,
    Nanos,
    Micros,
    Millis,
    Seconds,
    Minutes,
    Hours,
    Days,
    Now,
}

/// Lambda stores a function; its parameters and body are evaluated lazily.
/// Lambda's are created by evaluating the "Fn()" expression.
#[derive(Deserialize, Debug, Clone)]
pub struct Lambda {
    parameters: Vec<String>,
    body: Expression,
}

impl Lambda {
    fn valid_parameters(parameters: &Expression) -> Result<Vec<String>, MetricValue> {
        match parameters {
            Expression::Vector(parameters) => parameters
                .iter()
                .map(|param| match param {
                    Expression::Variable(name) => {
                        if name.includes_namespace() {
                            Err(MetricValue::Missing(
                                "Namespaces not allowed in function params".to_string(),
                            ))
                        } else {
                            Ok(name.original_name().to_string())
                        }
                    }
                    _ => Err(MetricValue::Missing(
                        "Function params must be valid identifier names".to_string(),
                    )),
                })
                .collect::<Result<Vec<_>, _>>(),
            _ => {
                return Err(MetricValue::Missing(
                    "Function params must be a vector of names".to_string(),
                ))
            }
        }
    }

    fn as_metric_value(definition: &Vec<Expression>) -> MetricValue {
        if definition.len() != 2 {
            return MetricValue::Missing(
                "Function needs two parameters, list of params and expression".to_string(),
            );
        }
        let parameters = match Self::valid_parameters(&definition[0]) {
            Ok(names) => names,
            Err(problem) => return problem,
        };
        let body = definition[1].clone();
        MetricValue::Lambda(Box::new(Lambda { parameters, body }))
    }
}

// Behavior for short circuiting execution when applying operands.
#[derive(Copy, Clone, Debug)]
enum ShortCircuitBehavior {
    // Short circuit when the first true value is found.
    True,
    // Short circuit when the first false value is found.
    False,
}

fn demand_numeric(value: &MetricValue) -> MetricValue {
    match value {
        MetricValue::Int(_) | MetricValue::Float(_) => {
            MetricValue::Missing("Internal bug - numeric passed to demand_numeric".to_string())
        }
        MetricValue::Missing(message) => MetricValue::Missing(message.clone()),
        other => MetricValue::Missing(format!("{} not numeric", other)),
    }
}

fn demand_both_numeric(value1: &MetricValue, value2: &MetricValue) -> MetricValue {
    match value1 {
        MetricValue::Float(_) | MetricValue::Int(_) => return demand_numeric(value2),
        _ => (),
    }
    match value2 {
        MetricValue::Float(_) | MetricValue::Int(_) => return demand_numeric(value1),
        _ => (),
    }
    let value1 = demand_numeric(value1);
    let value2 = demand_numeric(value2);
    MetricValue::Missing(format!("{} and {} not numeric", value1, value2))
}

/// Macro which handles applying a function to 2 operands and returns a
/// MetricValue.
///
/// The macro handles type promotion and promotion to the specified type.
macro_rules! apply_math_operands {
    ($left:expr, $right:expr, $function:expr, $ty:ty) => {
        match (unwrap_for_math(&$left), unwrap_for_math(&$right)) {
            (MetricValue::Int(int1), MetricValue::Int(int2)) => {
                // TODO(cphoenix): Instead of converting to float, use int functions.
                ($function(*int1 as f64, *int2 as f64) as $ty).into()
            }
            (MetricValue::Int(int1), MetricValue::Float(float2)) => {
                $function(*int1 as f64, *float2).into()
            }
            (MetricValue::Float(float1), MetricValue::Int(int2)) => {
                $function(*float1, *int2 as f64).into()
            }
            (MetricValue::Float(float1), MetricValue::Float(float2)) => {
                $function(*float1, *float2).into()
            }
            (value1, value2) => demand_both_numeric(value1, value2),
        }
    };
}

/// A macro which extracts two binary operands from a vec of operands and
/// applies the given function.
macro_rules! extract_and_apply_math_operands {
    ($self:ident, $namespace:expr, $function:expr, $operands:expr, $ty:ty) => {
        match MetricState::extract_binary_operands($self, $namespace, $operands) {
            Ok((left, right)) => apply_math_operands!(left, right, $function, $ty),
            Err(value) => value,
        }
    };
}

/// Expression represents the parsed body of an Eval Metric. It applies
/// a function to sub-expressions, or stores a Missing error, the name of a
/// Metric, a vector of expressions, or a basic Value.
#[derive(Deserialize, Debug, Clone, PartialEq)]
pub enum Expression {
    // Some operators have arity 1 or 2, some have arity N.
    // For symmetry/readability, I use the same operand-spec Vec<Expression> for all.
    // TODO(cphoenix): Check on load that all operators have a legal number of operands.
    Function(Function, Vec<Expression>),
    Vector(Vec<Expression>),
    Variable(VariableName),
    Value(MetricValue),
}

// Selectors return a vec of values. Typically they will select a single
// value which we want to use for math without lots of boilerplate.
// So we "promote" a 1-entry vector into the value it contains.
// Other vectors will be passed through unchanged (and cause an error later).
fn unwrap_for_math<'b>(value: &'b MetricValue) -> &'b MetricValue {
    match value {
        MetricValue::Vector(v) if v.len() == 1 => &v[0],
        v => v,
    }
}

// Condense the visual clutter of iter() and collect::<Vec<_>>().
fn map_vec<T, U, F>(vec: &Vec<T>, f: F) -> Vec<U>
where
    F: FnMut(&T) -> U,
{
    vec.iter().map(f).collect::<Vec<U>>()
}

// Condense visual clutter of iterating over vec of references.
fn map_vec_r<'a, T, U, F>(vec: &'a [T], f: F) -> Vec<&'a U>
where
    F: FnMut(&'a T) -> &'a U,
{
    vec.iter().map(f).collect::<Vec<&'a U>>()
}

// Construct Missing() metric from a message
fn missing(message: &str) -> MetricValue {
    MetricValue::Missing(message.to_string())
}

impl<'a> MetricState<'a> {
    /// Create an initialized MetricState.
    pub fn new(
        metrics: &'a Metrics,
        fetcher: Fetcher<'a>,
        time_source: &'a dyn TimeSource,
    ) -> MetricState<'a> {
        MetricState { metrics, fetcher, now: time_source.now() }
    }

    /// Any [name] found in the trial's "values" uses the corresponding value, regardless of
    /// whether it is a Selector or Eval Metric, and regardless of whether it includes
    /// a namespace; the string match must be exact.
    /// If not found in "values" the name must be an Eval metric from the current file.
    fn metric_value_for_trial(
        &self,
        fetcher: &TrialDataFetcher<'_>,
        namespace: &str,
        variable: &VariableName,
    ) -> MetricValue {
        let name = variable.original_name();
        if fetcher.has_entry(name) {
            return fetcher.fetch(name);
        }
        if variable.includes_namespace() {
            return MetricValue::Missing(format!(
                "Name {} not in test values and refers outside the file",
                name
            ));
        }
        match self.metrics.get(namespace) {
            None => return MetricValue::Missing(format!("BUG! Bad namespace '{}'", namespace)),
            Some(metric_map) => match metric_map.get(name) {
                None => {
                    return MetricValue::Missing(format!(
                        "Metric '{}' Not Found in '{}'",
                        name, namespace
                    ))
                }
                Some(metric) => match metric {
                    Metric::Selector(_) => MetricValue::Missing(format!(
                        "Selector {} can't be used in tests; please supply a value",
                        name
                    )),
                    Metric::Eval(expression) => self.evaluate_value(namespace, &expression),
                },
            },
        }
    }

    /// If [name] is of the form "namespace::name" then [namespace] is ignored.
    /// If [name] is just "name" then [namespace] is used to look up the Metric.
    fn metric_value_for_file(
        &self,
        fetcher: &FileDataFetcher<'_>,
        namespace: &str,
        name: &VariableName,
    ) -> MetricValue {
        if let Some((real_namespace, real_name)) = name.name_parts(namespace) {
            match self.metrics.get(real_namespace) {
                None => return MetricValue::Missing(format!("Bad namespace '{}'", real_namespace)),
                Some(metric_map) => match metric_map.get(real_name) {
                    None => {
                        return MetricValue::Missing(format!(
                            "Metric '{}' Not Found in '{}'",
                            real_name, real_namespace
                        ))
                    }
                    Some(metric) => match metric {
                        Metric::Selector(selector) => fetcher.fetch(selector),
                        Metric::Eval(expression) => {
                            self.evaluate_value(real_namespace, &expression)
                        }
                    },
                },
            }
        } else {
            return MetricValue::Missing(format!("Bad name '{}'", name.original_name()));
        }
    }

    /// Calculate the value of a Metric specified by name and namespace.
    fn evaluate_variable(&self, namespace: &str, name: &VariableName) -> MetricValue {
        // TODO(cphoenix): When historical metrics are added, change semantics to refresh()
        // TODO(cphoenix): cache values
        // TODO(cphoenix): Detect infinite cycles/depth.
        // TODO(cphoenix): Improve the data structure on Metric names. Probably fill in
        //  namespace during parse.
        match &self.fetcher {
            Fetcher::FileData(fetcher) => self.metric_value_for_file(fetcher, namespace, name),
            Fetcher::TrialData(fetcher) => self.metric_value_for_trial(fetcher, namespace, name),
        }
    }

    /// Fetch or compute the value of a Metric expression from an action.
    pub fn eval_action_metric(&self, namespace: &str, metric: &Metric) -> MetricValue {
        match metric {
            Metric::Selector(_) => {
                MetricValue::Missing("Selectors aren't allowed in action triggers".to_owned())
            }
            Metric::Eval(string) => {
                unwrap_for_math(&self.evaluate_value(namespace, string)).clone()
            }
        }
    }

    fn evaluate_value(&self, namespace: &str, expression: &str) -> MetricValue {
        match config::parse::parse_expression(expression) {
            Ok(expr) => self.evaluate(namespace, &expr),
            Err(e) => MetricValue::Missing(format!("Expression parse error\n{}", e)),
        }
    }

    /// Evaluate an Expression which contains only base values, not referring to other Metrics.
    pub fn evaluate_math(e: &Expression) -> MetricValue {
        let values = HashMap::new();
        let fetcher = Fetcher::TrialData(TrialDataFetcher::new(&values));
        let files = HashMap::new();
        let time_source = FakeTime::new();
        let metric_state = MetricState::new(&files, fetcher, &time_source);
        metric_state.evaluate(&"".to_string(), e)
    }

    #[cfg(test)]
    pub fn evaluate_expression(&self, e: &Expression) -> MetricValue {
        self.evaluate(&"".to_string(), e)
    }

    fn evaluate_function(
        &self,
        namespace: &str,
        function: &Function,
        operands: &Vec<Expression>,
    ) -> MetricValue {
        match function {
            Function::Add => self.fold_math(namespace, &|a, b| a + b, operands),
            Function::Sub => self.apply_math(namespace, &|a, b| a - b, operands),
            Function::Mul => self.fold_math(namespace, &|a, b| a * b, operands),
            Function::FloatDiv => self.apply_math_f(namespace, &|a, b| a / b, operands),
            Function::IntDiv => self.apply_math(namespace, &|a, b| f64::trunc(a / b), operands),
            Function::Greater => self.apply_cmp(namespace, &|a, b| a > b, operands),
            Function::Less => self.apply_cmp(namespace, &|a, b| a < b, operands),
            Function::GreaterEq => self.apply_cmp(namespace, &|a, b| a >= b, operands),
            Function::LessEq => self.apply_cmp(namespace, &|a, b| a <= b, operands),
            Function::Equals => self.apply_metric_cmp(namespace, &|a, b| a == b, operands),
            Function::NotEq => self.apply_metric_cmp(namespace, &|a, b| a != b, operands),
            Function::Max => self.fold_math(namespace, &|a, b| if a > b { a } else { b }, operands),
            Function::Min => self.fold_math(namespace, &|a, b| if a < b { a } else { b }, operands),
            Function::And => {
                self.fold_bool(namespace, &|a, b| a && b, operands, ShortCircuitBehavior::False)
            }
            Function::Or => {
                self.fold_bool(namespace, &|a, b| a || b, operands, ShortCircuitBehavior::True)
            }
            Function::Not => self.not_bool(namespace, operands),
            Function::KlogHas | Function::SyslogHas | Function::BootlogHas => {
                self.log_contains(function, namespace, operands)
            }
            Function::Missing => self.is_missing(namespace, operands),
            Function::Annotation => self.annotation(namespace, operands),
            Function::Lambda => Lambda::as_metric_value(operands),
            Function::Map => self.map(namespace, operands),
            Function::Fold => self.fold(namespace, operands),
            Function::Filter => self.filter(namespace, operands),
            Function::Count => self.count(namespace, operands),
            Function::Nanos => self.time(namespace, operands, 1),
            Function::Micros => self.time(namespace, operands, 1_000),
            Function::Millis => self.time(namespace, operands, 1_000_000),
            Function::Seconds => self.time(namespace, operands, 1_000_000_000),
            Function::Minutes => self.time(namespace, operands, 1_000_000_000 * 60),
            Function::Hours => self.time(namespace, operands, 1_000_000_000 * 60 * 60),
            Function::Days => self.time(namespace, operands, 1_000_000_000 * 60 * 60 * 24),
            Function::Now => self.now(operands),
        }
    }

    fn now(&self, operands: &'a [Expression]) -> MetricValue {
        if !operands.is_empty() {
            return missing("Now() requires no operands.");
        }
        MetricValue::Int(self.now)
    }

    fn apply_lambda(&self, namespace: &str, lambda: &Lambda, args: &[&MetricValue]) -> MetricValue {
        fn substitute_all(
            expressions: &[Expression],
            bindings: &HashMap<String, &MetricValue>,
        ) -> Vec<Expression> {
            expressions.iter().map(|e| substitute(e, bindings)).collect::<Vec<_>>()
        }

        fn substitute(
            expression: &Expression,
            bindings: &HashMap<String, &MetricValue>,
        ) -> Expression {
            match expression {
                Expression::Function(function, expressions) => {
                    Expression::Function(function.clone(), substitute_all(expressions, bindings))
                }
                Expression::Vector(expressions) => {
                    Expression::Vector(substitute_all(expressions, bindings))
                }
                Expression::Variable(name) => {
                    if let Some(value) = bindings.get(name.original_name()) {
                        Expression::Value((*value).clone())
                    } else {
                        Expression::Variable(name.clone())
                    }
                }
                Expression::Value(value) => Expression::Value(value.clone()),
            }
        }

        let parameters = &lambda.parameters;
        if parameters.len() != args.len() {
            return MetricValue::Missing(format!(
                "Function has {} parameters and needs {} arguments, but has {}.",
                parameters.len(),
                parameters.len(),
                args.len()
            ));
        }
        let mut bindings = HashMap::new();
        for (name, value) in parameters.iter().zip(args.iter()) {
            bindings.insert(name.clone(), value.clone());
        }
        let expression = substitute(&lambda.body, &bindings);
        self.evaluate(namespace, &expression)
    }

    fn unpack_lambda(
        &self,
        namespace: &str,
        operands: &'a [Expression],
    ) -> Result<(Box<Lambda>, Vec<MetricValue>), ()> {
        if operands.len() == 0 {
            return Err(());
        }
        let lambda = match self.evaluate(namespace, &operands[0]) {
            MetricValue::Lambda(lambda) => lambda,
            _ => return Err(()),
        };
        let arguments =
            operands[1..].iter().map(|expr| self.evaluate(namespace, expr)).collect::<Vec<_>>();
        Ok((lambda, arguments))
    }

    /// This implements the Map() function.
    fn map(&self, namespace: &str, operands: &[Expression]) -> MetricValue {
        let (lambda, arguments) = match self.unpack_lambda(namespace, operands) {
            Ok((lambda, arguments)) => (lambda, arguments),
            Err(()) => return missing("Map needs a function in its first argument."),
        };
        let vector_args = arguments
            .iter()
            .filter(|item| match item {
                MetricValue::Vector(_) => true,
                _ => false,
            })
            .collect::<Vec<_>>();
        let result_length = match vector_args.len() {
            0 => 0,
            _ => {
                let start = match vector_args[0] {
                    MetricValue::Vector(vec) => vec.len(),
                    _ => unreachable!(),
                };
                vector_args.iter().fold(start, |accum, item| {
                    min(
                        accum,
                        match item {
                            MetricValue::Vector(items) => items.len(),
                            _ => 0,
                        },
                    )
                })
            }
        };
        let mut result = Vec::new();
        for index in 0..result_length {
            let call_args = arguments
                .iter()
                .map(|arg| match arg {
                    MetricValue::Vector(vec) => &vec[index],
                    other => other,
                })
                .collect::<Vec<_>>();
            result.push(self.apply_lambda(namespace, &lambda, &call_args));
        }
        MetricValue::Vector(result)
    }

    /// This implements the Fold() function.
    fn fold(&self, namespace: &str, operands: &[Expression]) -> MetricValue {
        let (lambda, arguments) = match self.unpack_lambda(namespace, operands) {
            Ok((lambda, arguments)) => (lambda, arguments),
            Err(()) => return missing("Fold needs a function as the first argument."),
        };
        if arguments.is_empty() {
            return missing("Fold needs a second argument, a vector");
        }
        let vector = match &arguments[0] {
            MetricValue::Vector(items) => items,
            _ => return missing("Second argument of Fold must be a vector"),
        };
        let (first, rest) = match arguments.len() {
            1 => match vector.split_first() {
                Some(first_rest) => first_rest,
                None => return missing("Fold needs at least one value"),
            },
            2 => (&arguments[1], &vector[..]),
            _ => return missing("Fold needs (function, vec) or (function, vec, start)"),
        };
        let mut result = first.clone();
        for item in rest {
            result = self.apply_lambda(namespace, &lambda, &[&result, item]);
        }
        result
    }

    /// This implements the Filter() function.
    fn filter(&self, namespace: &str, operands: &[Expression]) -> MetricValue {
        let (lambda, arguments) = match self.unpack_lambda(namespace, operands) {
            Ok((lambda, arguments)) => (lambda, arguments),
            Err(()) => return missing("Filter needs a function"),
        };
        if arguments.len() != 1 {
            return missing("Filter needs (function, vector)");
        }
        let result = match &arguments[0] {
            MetricValue::Vector(items) => items
                .iter()
                .filter_map(|item| match self.apply_lambda(namespace, &lambda, &[&item]) {
                    MetricValue::Bool(true) => Some(item.clone()),
                    MetricValue::Bool(false) => None,
                    MetricValue::Missing(message) => Some(MetricValue::Missing(message.clone())),
                    bad_type => Some(MetricValue::Missing(format!(
                        "Bad value {:?} from filter function should be true, false, or Missing",
                        bad_type
                    ))),
                })
                .collect(),
            _ => return missing("Filter second argument must be a vector"),
        };
        MetricValue::Vector(result)
    }

    /// This implements the Count() function.
    fn count(&self, namespace: &str, operands: &[Expression]) -> MetricValue {
        if operands.len() != 1 {
            return missing("Count requires one argument, a vector");
        }
        match self.evaluate(namespace, &operands[0]) {
            MetricValue::Vector(items) => MetricValue::Int(items.len() as i64),
            bad => MetricValue::Missing(format!("Count only works on vectors, not {}", bad)),
        }
    }

    pub fn safe_float_to_int(float: f64) -> Option<i64> {
        if !float.is_finite() {
            return None;
        }
        if float > i64::MAX as f64 {
            return Some(i64::MAX);
        }
        if float < i64::MIN as f64 {
            return Some(i64::MIN);
        }
        Some(float as i64)
    }

    /// This implements the time-conversion functions.
    fn time(&self, namespace: &str, operands: &[Expression], multiplier: i64) -> MetricValue {
        if operands.len() != 1 {
            return missing("Time conversion needs 1 numeric argument");
        }
        match self.evaluate(namespace, &operands[0]) {
            MetricValue::Int(value) => MetricValue::Int(value * multiplier),
            MetricValue::Float(value) => {
                match Self::safe_float_to_int(value * (multiplier as f64)) {
                    None => missing("Time conversion needs 1 numeric argument"),
                    Some(value) => MetricValue::Int(value),
                }
            }
            _ => missing("Time conversion needs 1 numeric argument"),
        }
    }

    fn evaluate(&self, namespace: &str, e: &Expression) -> MetricValue {
        match e {
            Expression::Function(f, operands) => self.evaluate_function(namespace, f, operands),
            Expression::Variable(name) => self.evaluate_variable(namespace, name),
            Expression::Value(value) => value.clone(),
            Expression::Vector(values) => {
                MetricValue::Vector(map_vec(values, |value| self.evaluate(namespace, value)))
            }
        }
    }

    fn annotation(&self, namespace: &str, operands: &Vec<Expression>) -> MetricValue {
        if operands.len() != 1 {
            return MetricValue::Missing("Annotation() needs 1 string argument".to_string());
        }
        match self.evaluate(namespace, &operands[0]) {
            MetricValue::String(string) => match &self.fetcher {
                Fetcher::TrialData(fetcher) => fetcher.annotations,
                Fetcher::FileData(fetcher) => fetcher.annotations,
            }
            .fetch(&string),
            _ => MetricValue::Missing("Annotation() needs a string argument".to_string()),
        }
    }

    fn log_contains(
        &self,
        log_type: &Function,
        namespace: &str,
        operands: &Vec<Expression>,
    ) -> MetricValue {
        let log_data = match &self.fetcher {
            Fetcher::TrialData(fetcher) => match log_type {
                Function::KlogHas => fetcher.klog,
                Function::SyslogHas => fetcher.syslog,
                Function::BootlogHas => fetcher.bootlog,
                _ => {
                    return MetricValue::Missing(
                        "Internal error, log_contains with non-log function".to_string(),
                    )
                }
            },
            Fetcher::FileData(fetcher) => match log_type {
                Function::KlogHas => fetcher.klog,
                Function::SyslogHas => fetcher.syslog,
                Function::BootlogHas => fetcher.bootlog,
                _ => {
                    return MetricValue::Missing(
                        "Internal error, log_contains with non-log function".to_string(),
                    )
                }
            },
        };
        if operands.len() != 1 {
            return MetricValue::Missing(
                "Log matcher must use exactly 1 argument, an RE string.".into(),
            );
        }
        match self.evaluate(namespace, &operands[0]) {
            MetricValue::String(re) => MetricValue::Bool(log_data.contains(&re)),
            _ => MetricValue::Missing("Log matcher needs a string (RE).".into()),
        }
    }

    // Applies an operator (which should be associative and commutative) to a list of operands.
    fn fold_math(
        &self,
        namespace: &str,
        function: &dyn (Fn(f64, f64) -> f64),
        operands: &Vec<Expression>,
    ) -> MetricValue {
        if operands.len() == 0 {
            return MetricValue::Missing("No operands in math expression".into());
        }
        let mut result: MetricValue = self.evaluate(namespace, &operands[0]);
        for operand in operands[1..].iter() {
            result = self.apply_math(
                namespace,
                function,
                &vec![Expression::Value(result), operand.clone()],
            );
        }
        result
    }

    // Applies a given function to two values, handling type-promotion.
    // This function will return a MetricValue::Int if all values are ints
    // and a MetricValue::Float if any argument is a float.
    fn apply_math(
        &self,
        namespace: &str,
        function: &dyn (Fn(f64, f64) -> f64),
        operands: &Vec<Expression>,
    ) -> MetricValue {
        extract_and_apply_math_operands!(self, namespace, function, operands, i64)
    }

    // Applies a given function to two values, handling type-promotion.
    // This function will always return a MetricValue::Float
    fn apply_math_f(
        &self,
        namespace: &str,
        function: &dyn (Fn(f64, f64) -> f64),
        operands: &Vec<Expression>,
    ) -> MetricValue {
        extract_and_apply_math_operands!(self, namespace, function, operands, f64)
    }

    fn extract_binary_operands(
        &self,
        namespace: &str,
        operands: &Vec<Expression>,
    ) -> Result<(MetricValue, MetricValue), MetricValue> {
        if operands.len() != 2 {
            return Err(MetricValue::Missing(format!(
                "Bad arg list {:?} for binary operator",
                operands
            )));
        }
        Ok((self.evaluate(namespace, &operands[0]), self.evaluate(namespace, &operands[1])))
    }

    // Applies an ord operator to two numbers. (>, >=, <, <=)
    fn apply_cmp(
        &self,
        namespace: &str,
        function: &dyn (Fn(f64, f64) -> bool),
        operands: &Vec<Expression>,
    ) -> MetricValue {
        if operands.len() != 2 {
            return MetricValue::Missing(format!(
                "Bad arg list {:?} for binary operator",
                operands
            ));
        }
        let operand_values = map_vec(&operands, |operand| self.evaluate(namespace, operand));
        let numbers = map_vec_r(&operand_values, |operand| unwrap_for_math(operand));
        let result = match (numbers[0], numbers[1]) {
            // TODO(cphoenix): Instead of converting two ints to float, use int functions.
            (MetricValue::Int(int1), MetricValue::Int(int2)) => {
                function(*int1 as f64, *int2 as f64)
            }
            (MetricValue::Int(int1), MetricValue::Float(float2)) => function(*int1 as f64, *float2),
            (MetricValue::Float(float1), MetricValue::Int(int2)) => function(*float1, *int2 as f64),
            (MetricValue::Float(float1), MetricValue::Float(float2)) => function(*float1, *float2),
            (value1, value2) => return demand_both_numeric(value1, value2),
        };
        MetricValue::Bool(result)
    }

    // Transitional Function to allow for string equality comparisons.
    // This function will eventually replace the apply_cmp function once MetricValue
    // implements the std::cmp::PartialOrd trait
    fn apply_metric_cmp(
        &self,
        namespace: &str,
        function: &dyn (Fn(&MetricValue, &MetricValue) -> bool),
        operands: &Vec<Expression>,
    ) -> MetricValue {
        if operands.len() != 2 {
            return MetricValue::Missing(format!(
                "Bad arg list {:?} for binary operator",
                operands
            ));
        }
        let operand_values = map_vec(&operands, |operand| self.evaluate(namespace, operand));
        let bools = map_vec_r(&operand_values, |operand| unwrap_for_math(operand));
        match (bools[0], bools[1]) {
            // We forward ::Missing for better error messaging.
            (MetricValue::Missing(reason), _) => MetricValue::Missing(reason.to_string()),
            (_, MetricValue::Missing(reason)) => MetricValue::Missing(reason.to_string()),
            _ => MetricValue::Bool(function(bools[0], bools[1])),
        }
    }

    fn fold_bool(
        &self,
        namespace: &str,
        function: &dyn (Fn(bool, bool) -> bool),
        operands: &Vec<Expression>,
        short_circuit_behavior: ShortCircuitBehavior,
    ) -> MetricValue {
        if operands.len() == 0 {
            return MetricValue::Missing("No operands in boolean expression".into());
        }
        let first = self.evaluate(namespace, &operands[0]);
        let mut result: bool = match unwrap_for_math(&first) {
            MetricValue::Bool(value) => *value,
            MetricValue::Missing(reason) => {
                return MetricValue::Missing(reason.to_string());
            }
            bad => return MetricValue::Missing(format!("{:?} is not boolean", bad)),
        };
        for operand in operands[1..].iter() {
            match (result, short_circuit_behavior) {
                (true, ShortCircuitBehavior::True) => {
                    break;
                }
                (false, ShortCircuitBehavior::False) => {
                    break;
                }
                _ => {}
            };
            let nth = self.evaluate(namespace, operand);
            result = match unwrap_for_math(&nth) {
                MetricValue::Bool(value) => function(result, *value),
                MetricValue::Missing(reason) => {
                    return MetricValue::Missing(reason.to_string());
                }
                bad => return MetricValue::Missing(format!("{:?} is not boolean", bad)),
            }
        }
        MetricValue::Bool(result)
    }

    fn not_bool(&self, namespace: &str, operands: &Vec<Expression>) -> MetricValue {
        if operands.len() != 1 {
            return MetricValue::Missing(format!(
                "Wrong number of arguments ({}) for unary bool operator",
                operands.len()
            ));
        }
        match unwrap_for_math(&self.evaluate(namespace, &operands[0])) {
            MetricValue::Bool(true) => MetricValue::Bool(false),
            MetricValue::Bool(false) => MetricValue::Bool(true),
            MetricValue::Missing(reason) => {
                return MetricValue::Missing(reason.to_string());
            }
            bad => return MetricValue::Missing(format!("{:?} not boolean", bad)),
        }
    }

    // Returns Bool true if the given metric is Missing, false if the metric has a value.
    fn is_missing(&self, namespace: &str, operands: &Vec<Expression>) -> MetricValue {
        if operands.len() != 1 {
            return MetricValue::Missing(format!("Bad operand"));
        }
        MetricValue::Bool(match self.evaluate(namespace, &operands[0]) {
            MetricValue::Missing(_) => true,
            // TODO(fxbug.dev/58922): Well-designed errors and special cases, not hacks
            MetricValue::Vector(contents) if contents.len() == 0 => true,
            MetricValue::Vector(contents) if contents.len() == 1 => match contents[0] {
                MetricValue::Missing(_) => true,
                _ => false,
            },
            _ => false,
        })
    }
}

// The evaluation of math expressions is tested pretty exhaustively in parse.rs unit tests.

// The use of metric names in expressions and actions, with and without namespaces, is tested in
// the integration test.
//   $ fx test triage_lib_test

#[cfg(test)]
pub(crate) mod test {
    use super::*;
    use {
        anyhow::Error,
        lazy_static::lazy_static,
        serde_json::{json, Number as JsonNumber},
    };

    /// Missing should never equal anything, even an identical Missing. Code (tests) can use
    /// assert_missing!(MetricValue::Missing("foo".to_string()), "foo") to test error messages.
    #[macro_export]
    macro_rules! assert_missing {
        ($missing:expr, $message:expr) => {
            match $missing {
                MetricValue::Missing(actual_message) => assert_eq!(&actual_message, $message),
                _ => assert!(false, "Non-Missing type"),
            }
        };
    }

    #[test]
    fn test_equality() {
        // Equal Value, Equal Type
        assert_eq!(MetricValue::Int(1), MetricValue::Int(1));
        assert_eq!(MetricValue::Float(1.0), MetricValue::Float(1.0));
        assert_eq!(MetricValue::String("A".to_string()), MetricValue::String("A".to_string()));
        assert_eq!(MetricValue::Bool(true), MetricValue::Bool(true));
        assert_eq!(MetricValue::Bool(false), MetricValue::Bool(false));
        assert_eq!(
            MetricValue::Vector(vec![
                MetricValue::Int(1),
                MetricValue::Float(1.0),
                MetricValue::String("A".to_string()),
                MetricValue::Bool(true),
            ]),
            MetricValue::Vector(vec![
                MetricValue::Int(1),
                MetricValue::Float(1.0),
                MetricValue::String("A".to_string()),
                MetricValue::Bool(true),
            ])
        );
        assert_eq!(MetricValue::Bytes(vec![1, 2, 3]), MetricValue::Bytes(vec![1, 2, 3]));

        // Floats and ints interconvert. Test both ways for full code coverage.
        assert_eq!(MetricValue::Int(1), MetricValue::Float(1.0));
        assert_eq!(MetricValue::Float(1.0), MetricValue::Int(1));

        // Numbers, vectors, and byte arrays do not interconvert when compared with Rust ==.
        // Note, though, that the expression "1 == [1]" will evaluate to true.
        assert!(MetricValue::Int(1) != MetricValue::Vector(vec![MetricValue::Int(1)]));
        assert!(MetricValue::Bytes(vec![1]) != MetricValue::Vector(vec![MetricValue::Int(1)]));
        assert!(MetricValue::Int(1) != MetricValue::Bytes(vec![1]));

        // Nested array
        assert_eq!(
            MetricValue::Vector(vec![
                MetricValue::Int(1),
                MetricValue::Float(1.0),
                MetricValue::String("A".to_string()),
                MetricValue::Bool(true),
            ]),
            MetricValue::Vector(vec![
                MetricValue::Int(1),
                MetricValue::Float(1.0),
                MetricValue::String("A".to_string()),
                MetricValue::Bool(true),
            ])
        );

        // Missing should never be equal
        assert!(MetricValue::Missing("err".to_string()) != MetricValue::Missing("err".to_string()));
        // Use assert_missing() macro to test error messages.
        assert_missing!(MetricValue::Missing("err".to_string()), "err");

        // We don't have a contract for Lambda equality. We probably don't need one.
    }

    #[test]
    fn test_inequality() {
        // Different Value, Equal Type
        assert_ne!(MetricValue::Int(1), MetricValue::Int(2));
        assert_ne!(MetricValue::Float(1.0), MetricValue::Float(2.0));
        assert_ne!(MetricValue::String("A".to_string()), MetricValue::String("B".to_string()));
        assert_ne!(MetricValue::Bool(true), MetricValue::Bool(false));
        assert_ne!(
            MetricValue::Vector(vec![
                MetricValue::Int(1),
                MetricValue::Float(1.0),
                MetricValue::String("A".to_string()),
                MetricValue::Bool(true),
            ]),
            MetricValue::Vector(vec![
                MetricValue::Int(2),
                MetricValue::Float(2.0),
                MetricValue::String("B".to_string()),
                MetricValue::Bool(false),
            ])
        );

        // Different Type
        assert_ne!(MetricValue::Int(2), MetricValue::Float(1.0));
        assert_ne!(MetricValue::Int(1), MetricValue::String("A".to_string()));
        assert_ne!(MetricValue::Int(1), MetricValue::Bool(true));
        assert_ne!(MetricValue::Float(1.0), MetricValue::String("A".to_string()));
        assert_ne!(MetricValue::Float(1.0), MetricValue::Bool(true));
        assert_ne!(MetricValue::String("A".to_string()), MetricValue::Bool(true));
    }

    #[test]
    fn test_fmt() {
        assert_eq!(format!("{}", MetricValue::Int(3)), "Int(3)");
        assert_eq!(format!("{}", MetricValue::Float(3.5)), "Float(3.5)");
        assert_eq!(format!("{}", MetricValue::Bool(true)), "Bool(true)");
        assert_eq!(format!("{}", MetricValue::Bool(false)), "Bool(false)");
        assert_eq!(format!("{}", MetricValue::String("cat".to_string())), "String(cat)");
        assert_eq!(
            format!("{}", MetricValue::Vector(vec![MetricValue::Int(1), MetricValue::Float(2.5)])),
            "Vector([Int(1), Float(2.5)])"
        );
        assert_eq!(format!("{}", MetricValue::Bytes(vec![1u8, 2u8])), "Bytes([1, 2])");
        assert_eq!(
            format!("{}", MetricValue::Missing("Where is Waldo?".to_string())),
            "Missing(Where is Waldo?)"
        );
    }

    lazy_static! {
        static ref LOCAL_M: HashMap<String, JsonValue> = {
            let mut m = HashMap::new();
            m.insert("foo".to_owned(), JsonValue::try_from(42).unwrap());
            m.insert("a::b".to_owned(), JsonValue::try_from(7).unwrap());
            m
        };
        static ref FOO_42_AB_7_TRIAL_FETCHER: TrialDataFetcher<'static> =
            TrialDataFetcher::new(&LOCAL_M);
        static ref LOCAL_F: Vec<DiagnosticData> = {
            let s = r#"[{
                "data_source": "Inspect",
                "moniker": "bar.cmx",
                "payload": { "root": { "bar": 99 }}
            }]"#;
            vec![DiagnosticData::new("i".to_string(), Source::Inspect, s.to_string()).unwrap()]
        };
        static ref EMPTY_F: Vec<DiagnosticData> = {
            let s = r#"[]"#;
            vec![DiagnosticData::new("i".to_string(), Source::Inspect, s.to_string()).unwrap()]
        };
        static ref NO_PAYLOAD_F: Vec<DiagnosticData> = {
            let s = r#"[{"moniker": "abcd", "payload": null}]"#;
            vec![DiagnosticData::new("i".to_string(), Source::Inspect, s.to_string()).unwrap()]
        };
        static ref BAR_99_FILE_FETCHER: FileDataFetcher<'static> = FileDataFetcher::new(&LOCAL_F);
        static ref EMPTY_FILE_FETCHER: FileDataFetcher<'static> = FileDataFetcher::new(&EMPTY_F);
        static ref NO_PAYLOAD_FETCHER: FileDataFetcher<'static> =
            FileDataFetcher::new(&NO_PAYLOAD_F);
        static ref BAR_SELECTOR: SelectorString =
            SelectorString::try_from("INSPECT:bar.cmx:root:bar".to_owned()).unwrap();
        static ref WRONG_SELECTOR: SelectorString =
            SelectorString::try_from("INSPECT:bar.cmx:root:oops".to_owned()).unwrap();
    }

    // Unlike the assert_missing macro, this matches any Missing() regardless of its payload
    // message. It flags `message` if `value` is not Missing(_).
    fn require_missing(value: MetricValue, message: &'static str) {
        match value {
            MetricValue::Missing(_) => {}
            _ => assert!(false, message),
        }
    }

    #[test]
    fn test_file_fetch() {
        assert_eq!(
            BAR_99_FILE_FETCHER.fetch(&BAR_SELECTOR),
            MetricValue::Vector(vec![MetricValue::Int(99)])
        );
        assert_eq!(BAR_99_FILE_FETCHER.fetch(&WRONG_SELECTOR), MetricValue::Vector(vec![]),);
    }

    #[test]
    fn test_trial_fetch() {
        assert!(FOO_42_AB_7_TRIAL_FETCHER.has_entry("foo"));
        assert!(FOO_42_AB_7_TRIAL_FETCHER.has_entry("a::b"));
        assert!(!FOO_42_AB_7_TRIAL_FETCHER.has_entry("a:b"));
        assert!(!FOO_42_AB_7_TRIAL_FETCHER.has_entry("oops"));
        assert_eq!(FOO_42_AB_7_TRIAL_FETCHER.fetch("foo"), MetricValue::Int(42));
        require_missing(
            FOO_42_AB_7_TRIAL_FETCHER.fetch("oops"),
            "Trial fetcher found bogus selector",
        );
    }

    macro_rules! variable {
        ($name:expr) => {
            &VariableName::new($name.to_string())
        };
    }

    #[test]
    fn test_eval_with_file() {
        let mut file_map = HashMap::new();
        file_map.insert("bar".to_owned(), Metric::Selector(BAR_SELECTOR.clone()));
        file_map.insert("bar_plus_one".to_owned(), Metric::Eval("bar+1".to_owned()));
        file_map.insert("oops_plus_one".to_owned(), Metric::Eval("oops+1".to_owned()));
        let mut other_file_map = HashMap::new();
        other_file_map.insert("bar".to_owned(), Metric::Eval("42".to_owned()));
        let mut metrics = HashMap::new();
        metrics.insert("bar_file".to_owned(), file_map);
        metrics.insert("other_file".to_owned(), other_file_map);
        let fake_time = FakeTime::new();
        let file_state =
            MetricState::new(&metrics, Fetcher::FileData(BAR_99_FILE_FETCHER.clone()), &fake_time);
        assert_eq!(
            file_state.evaluate_variable("bar_file", variable!("bar_plus_one")),
            MetricValue::Int(100)
        );
        require_missing(
            file_state.evaluate_variable("bar_file", variable!("oops_plus_one")),
            "File found nonexistent name",
        );
        assert_eq!(
            file_state.evaluate_variable("bar_file", variable!("bar")),
            MetricValue::Vector(vec![MetricValue::Int(99)])
        );
        assert_eq!(
            file_state.evaluate_variable("other_file", variable!("bar")),
            MetricValue::Int(42)
        );
        assert_eq!(
            file_state.evaluate_variable("other_file", variable!("other_file::bar")),
            MetricValue::Int(42)
        );
        assert_eq!(
            file_state.evaluate_variable("other_file", variable!("bar_file::bar")),
            MetricValue::Vector(vec![MetricValue::Int(99)])
        );
        require_missing(
            file_state.evaluate_variable("other_file", variable!("bar_plus_one")),
            "Shouldn't have found bar_plus_one in other_file",
        );
        require_missing(
            file_state.evaluate_variable("missing_file", variable!("bar_plus_one")),
            "Shouldn't have found bar_plus_one in missing_file",
        );
        require_missing(
            file_state.evaluate_variable("bar_file", variable!("other_file::bar_plus_one")),
            "Shouldn't have found other_file::bar_plus_one",
        );
    }

    #[test]
    fn test_eval_with_trial() {
        let mut trial_map = HashMap::new();
        // The (broken) "foo" selector should be ignored in favor of the "foo" fetched value.
        trial_map.insert("foo".to_owned(), Metric::Selector(BAR_SELECTOR.clone()));
        trial_map.insert("foo_plus_one".to_owned(), Metric::Eval("foo+1".to_owned()));
        trial_map.insert("oops_plus_one".to_owned(), Metric::Eval("oops+1".to_owned()));
        trial_map.insert("ab_plus_one".to_owned(), Metric::Eval("a::b+1".to_owned()));
        trial_map.insert("ac_plus_one".to_owned(), Metric::Eval("a::c+1".to_owned()));
        // The file "a" should be completely ignored when testing foo_file.
        let mut a_map = HashMap::new();
        a_map.insert("b".to_owned(), Metric::Eval("2".to_owned()));
        a_map.insert("c".to_owned(), Metric::Eval("3".to_owned()));
        a_map.insert("foo".to_owned(), Metric::Eval("4".to_owned()));
        let mut metrics = HashMap::new();
        metrics.insert("foo_file".to_owned(), trial_map);
        metrics.insert("a".to_owned(), a_map);
        let fake_time = FakeTime::new();
        let trial_state = MetricState::new(
            &metrics,
            Fetcher::TrialData(FOO_42_AB_7_TRIAL_FETCHER.clone()),
            &fake_time,
        );
        // foo from values shadows foo selector.
        assert_eq!(
            trial_state.evaluate_variable("foo_file", variable!("foo")),
            MetricValue::Int(42)
        );
        // Value shadowing also works in expressions.
        assert_eq!(
            trial_state.evaluate_variable("foo_file", variable!("foo_plus_one")),
            MetricValue::Int(43)
        );
        // foo can shadow eval as well as selector.
        assert_eq!(trial_state.evaluate_variable("a", variable!("foo")), MetricValue::Int(42));
        // A value that's not there should be "Missing" (e.g. not crash)
        require_missing(
            trial_state.evaluate_variable("foo_file", variable!("oops_plus_one")),
            "Trial found nonexistent name",
        );
        // a::b ignores the "b" in file "a" and uses "a::b" from values.
        assert_eq!(
            trial_state.evaluate_variable("foo_file", variable!("ab_plus_one")),
            MetricValue::Int(8)
        );
        // a::c should return Missing, not look up c in file a.
        require_missing(
            trial_state.evaluate_variable("foo_file", variable!("ac_plus_one")),
            "Trial should not have read c from file a",
        );
    }

    #[test]
    fn logs_work() -> Result<(), Error> {
        let syslog_text = "line 1\nline 2\nsyslog".to_string();
        let klog_text = "first line\nsecond line\nklog\n".to_string();
        let bootlog_text = "Yes there's a bootlog with one long line".to_string();
        let syslog = DiagnosticData::new("sys".to_string(), Source::Syslog, syslog_text)?;
        let klog = DiagnosticData::new("k".to_string(), Source::Klog, klog_text)?;
        let bootlog = DiagnosticData::new("boot".to_string(), Source::Bootlog, bootlog_text)?;
        let metrics = HashMap::new();
        let mut data = vec![klog, syslog, bootlog];
        let fetcher = FileDataFetcher::new(&data);
        let fake_time = FakeTime::new();
        let state = MetricState::new(&metrics, Fetcher::FileData(fetcher), &fake_time);
        assert_eq!(state.evaluate_value("", r#"KlogHas("lin")"#), MetricValue::Bool(true));
        assert_eq!(state.evaluate_value("", r#"KlogHas("l.ne")"#), MetricValue::Bool(true));
        assert_eq!(state.evaluate_value("", r#"KlogHas("fi.*ne")"#), MetricValue::Bool(true));
        assert_eq!(state.evaluate_value("", r#"KlogHas("fi.*sec")"#), MetricValue::Bool(false));
        assert_eq!(state.evaluate_value("", r#"KlogHas("first line")"#), MetricValue::Bool(true));
        // Full regex; even capture groups are allowed but the values can't be extracted.
        assert_eq!(
            state.evaluate_value("", r#"KlogHas("f(.)rst \bline")"#),
            MetricValue::Bool(true)
        );
        // Backreferences don't work; this is regex, not fancy_regex.
        assert_eq!(
            state.evaluate_value("", r#"KlogHas("f(.)rst \bl\1ne")"#),
            MetricValue::Bool(false)
        );
        assert_eq!(state.evaluate_value("", r#"KlogHas("second line")"#), MetricValue::Bool(true));
        assert_eq!(
            state.evaluate_value("", "KlogHas(\"second line\n\")"),
            MetricValue::Bool(false)
        );
        assert_eq!(state.evaluate_value("", r#"KlogHas("klog")"#), MetricValue::Bool(true));
        assert_eq!(state.evaluate_value("", r#"KlogHas("line 2")"#), MetricValue::Bool(false));
        assert_eq!(state.evaluate_value("", r#"SyslogHas("line 2")"#), MetricValue::Bool(true));
        assert_eq!(state.evaluate_value("", r#"SyslogHas("syslog")"#), MetricValue::Bool(true));
        assert_eq!(state.evaluate_value("", r#"BootlogHas("bootlog")"#), MetricValue::Bool(true));
        assert_eq!(state.evaluate_value("", r#"BootlogHas("syslog")"#), MetricValue::Bool(false));
        data.pop();
        let fetcher = FileDataFetcher::new(&data);
        let fake_time = FakeTime::new();
        let state = MetricState::new(&metrics, Fetcher::FileData(fetcher), &fake_time);
        assert_eq!(state.evaluate_value("", r#"SyslogHas("syslog")"#), MetricValue::Bool(true));
        assert_eq!(state.evaluate_value("", r#"BootlogHas("bootlog")"#), MetricValue::Bool(false));
        assert_eq!(state.evaluate_value("", r#"BootlogHas("syslog")"#), MetricValue::Bool(false));
        Ok(())
    }

    #[test]
    fn annotations_work() -> Result<(), Error> {
        let annotation_text = r#"{ "build.board": "chromebook-x64", "answer": 42 }"#.to_string();
        let annotations =
            DiagnosticData::new("a".to_string(), Source::Annotations, annotation_text)?;
        let metrics = HashMap::new();
        let data = vec![annotations];
        let fetcher = FileDataFetcher::new(&data);
        let fake_time = FakeTime::new();
        let state = MetricState::new(&metrics, Fetcher::FileData(fetcher), &fake_time);
        assert_eq!(
            state.evaluate_value("", "Annotation('build.board')"),
            MetricValue::String("chromebook-x64".to_string())
        );
        assert_eq!(state.evaluate_value("", "Annotation('answer')"), MetricValue::Int(42));
        assert_missing!(
            state.evaluate_value("", "Annotation('bogus')"),
            "Key 'bogus' not found in annotations"
        );
        assert_missing!(
            state.evaluate_value("", "Annotation('bogus', 'Double bogus')"),
            "Annotation() needs 1 string argument"
        );
        assert_missing!(
            state.evaluate_value("", "Annotation(42)"),
            "Annotation() needs a string argument"
        );
        Ok(())
    }

    #[test]
    fn test_fetch_errors() {
        assert_eq!(1, NO_PAYLOAD_FETCHER.errors().len());
    }

    // Correct operation of the klog, syslog, and bootlog fields of TrialDataFetcher are tested
    // in the integration test via log_tests.triage.

    // Test evaluation on static values.
    #[test]
    fn test_evaluation() {
        let metrics: Metrics = [(
            "root".to_string(),
            [
                ("is42".to_string(), Metric::Eval("42".to_string())),
                ("isOk".to_string(), Metric::Eval("'OK'".to_string())),
            ]
            .iter()
            .cloned()
            .collect(),
        )]
        .iter()
        .cloned()
        .collect();
        let fake_time = FakeTime::new();
        let state =
            MetricState::new(&metrics, Fetcher::FileData(EMPTY_FILE_FETCHER.clone()), &fake_time);

        // Can read a value.
        assert_eq!(state.evaluate_value("root", "is42"), MetricValue::Int(42));

        // Basic arithmetic
        assert_eq!(state.evaluate_value("root", "is42 + 1"), MetricValue::Int(43));
        assert_eq!(state.evaluate_value("root", "is42 - 1"), MetricValue::Int(41));
        assert_eq!(state.evaluate_value("root", "is42 * 2"), MetricValue::Int(84));
        // Automatic float conversion and truncating divide.
        assert_eq!(state.evaluate_value("root", "is42 / 4"), MetricValue::Float(10.5));
        assert_eq!(state.evaluate_value("root", "is42 // 4"), MetricValue::Int(10));

        // Order of operations
        assert_eq!(
            state.evaluate_value("root", "is42 + 10 / 2 * 10 - 2 "),
            MetricValue::Float(90.0)
        );
        assert_eq!(state.evaluate_value("root", "is42 + 10 // 2 * 10 - 2 "), MetricValue::Int(90));

        // Boolean
        assert_eq!(
            state.evaluate_value("root", "And(is42 == 42, is42 < 100)"),
            MetricValue::Bool(true)
        );
        assert_eq!(
            state.evaluate_value("root", "And(is42 == 42, is42 > 100)"),
            MetricValue::Bool(false)
        );
        assert_eq!(
            state.evaluate_value("root", "Or(is42 == 42, is42 > 100)"),
            MetricValue::Bool(true)
        );
        assert_eq!(
            state.evaluate_value("root", "Or(is42 != 42, is42 < 100)"),
            MetricValue::Bool(true)
        );
        assert_eq!(
            state.evaluate_value("root", "Or(is42 != 42, is42 > 100)"),
            MetricValue::Bool(false)
        );
        assert_eq!(state.evaluate_value("root", "Not(is42 == 42)"), MetricValue::Bool(false));

        // Read strings
        assert_eq!(state.evaluate_value("root", "isOk"), MetricValue::String("OK".to_string()));

        // Missing value
        assert_missing!(
            state.evaluate_value("root", "missing"),
            "Metric 'missing' Not Found in 'root'"
        );

        // Booleans short circuit
        assert_missing!(
            state.evaluate_value("root", "Or(is42 != 42, missing)"),
            "Metric 'missing' Not Found in 'root'"
        );
        assert_eq!(
            state.evaluate_value("root", "Or(is42 == 42, missing)"),
            MetricValue::Bool(true)
        );
        assert_missing!(
            state.evaluate_value("root", "And(is42 == 42, missing)"),
            "Metric 'missing' Not Found in 'root'"
        );

        assert_eq!(
            state.evaluate_value("root", "And(is42 != 42, missing)"),
            MetricValue::Bool(false)
        );

        // Missing checks
        assert_eq!(state.evaluate_value("root", "Missing(is42)"), MetricValue::Bool(false));
        assert_eq!(state.evaluate_value("root", "Missing(missing)"), MetricValue::Bool(true));
        assert_eq!(
            state.evaluate_value("root", "And(Not(Missing(is42)), is42 == 42)"),
            MetricValue::Bool(true)
        );
        assert_eq!(
            state.evaluate_value("root", "And(Not(Missing(missing)), missing == 'Hello')"),
            MetricValue::Bool(false)
        );
        assert_eq!(
            state.evaluate_value("root", "Or(Missing(is42), is42 < 42)"),
            MetricValue::Bool(false)
        );
        assert_eq!(
            state.evaluate_value("root", "Or(Missing(missing), missing == 'Hello')"),
            MetricValue::Bool(true)
        );

        // Ensure evaluation for action converts vector values.
        assert_eq!(
            state.evaluate_value("root", "[0==0]"),
            MetricValue::Vector(vec![MetricValue::Bool(true)])
        );
        assert_eq!(
            state.eval_action_metric("root", &Metric::Eval("[0==0]".to_string())),
            MetricValue::Bool(true)
        );

        assert_eq!(
            state.evaluate_value("root", "[0==0, 0==0]"),
            MetricValue::Vector(vec![MetricValue::Bool(true), MetricValue::Bool(true)])
        );
        assert_eq!(
            state.eval_action_metric("root", &Metric::Eval("[0==0, 0==0]".to_string())),
            MetricValue::Vector(vec![MetricValue::Bool(true), MetricValue::Bool(true)])
        );
    }

    #[test]
    fn metric_value_from_json() {
        /*
            JSON subtypes:
                Bool(bool),
                Number(Number),
                String(String),
                Array(Vec<Value>),
                Object(Map<String, Value>),
        */
        macro_rules! test_from {
            ($json:path, $metric:path, $value:expr) => {
                test_from_to!($json, $metric, $value, $value);
            };
        }
        macro_rules! test_from_int {
            ($json:path, $metric:path, $value:expr) => {
                test_from_to!($json, $metric, JsonNumber::from($value), $value);
            };
        }
        macro_rules! test_from_float {
            ($json:path, $metric:path, $value:expr) => {
                test_from_to!($json, $metric, JsonNumber::from_f64($value).unwrap(), $value);
            };
        }
        macro_rules! test_from_to {
            ($json:path, $metric:path, $json_value:expr, $metric_value:expr) => {
                let metric_value = $metric($metric_value);
                let json_value = $json($json_value);
                assert_eq!(metric_value, MetricValue::from(json_value));
            };
        }
        test_from!(JsonValue::String, MetricValue::String, "Hi World".to_string());
        test_from_int!(JsonValue::Number, MetricValue::Int, 3);
        test_from_int!(JsonValue::Number, MetricValue::Int, std::i64::MAX);
        test_from_int!(JsonValue::Number, MetricValue::Int, std::i64::MIN);
        test_from_to!(JsonValue::Number, MetricValue::Int, JsonNumber::from(std::u64::MAX), -1);
        test_from_float!(JsonValue::Number, MetricValue::Float, 3.14);
        test_from_float!(JsonValue::Number, MetricValue::Float, std::f64::MAX);
        test_from_float!(JsonValue::Number, MetricValue::Float, std::f64::MIN);
        test_from!(JsonValue::Bool, MetricValue::Bool, true);
        test_from!(JsonValue::Bool, MetricValue::Bool, false);
        let json_vec = vec![json!(1), json!(2), json!(3)];
        let metric_vec = vec![MetricValue::Int(1), MetricValue::Int(2), MetricValue::Int(3)];
        test_from_to!(JsonValue::Array, MetricValue::Vector, json_vec, metric_vec);
        assert_missing!(
            MetricValue::from(JsonValue::Object(serde_json::Map::new())),
            "Unsupported JSON type"
        );
    }

    #[test]
    fn metric_value_from_diagnostic_property() {
        /*
            DiagnosticProperty subtypes:
                String(Key, String),
                Bytes(Key, Vec<u8>),
                Int(Key, i64),
                Uint(Key, u64),
                Double(Key, f64),
                Bool(Key, bool),
                DoubleArray(Key, ArrayContent<f64>),
                IntArray(Key, ArrayContent<i64>),
                UintArray(Key, ArrayContent<u64>),
        */
        macro_rules! test_from {
            ($diagnostic:path, $metric:path, $value:expr) => {
                test_from_to!($diagnostic, $metric, $value, $value);
            };
        }
        macro_rules! test_from_to {
            ($diagnostic:path, $metric:path, $diagnostic_value:expr, $metric_value:expr) => {
                assert_eq!(
                    $metric($metric_value),
                    MetricValue::from($diagnostic("foo".to_string(), $diagnostic_value))
                );
            };
        }
        test_from!(DiagnosticProperty::String, MetricValue::String, "Hi World".to_string());
        test_from!(DiagnosticProperty::Bytes, MetricValue::Bytes, vec![1, 2, 3]);
        test_from!(DiagnosticProperty::Int, MetricValue::Int, 3);
        test_from!(DiagnosticProperty::Int, MetricValue::Int, std::i64::MAX);
        test_from!(DiagnosticProperty::Int, MetricValue::Int, std::i64::MIN);
        test_from!(DiagnosticProperty::Uint, MetricValue::Int, 3);
        test_from_to!(DiagnosticProperty::Uint, MetricValue::Int, std::u64::MAX, -1);
        test_from!(DiagnosticProperty::Double, MetricValue::Float, 3.14);
        test_from!(DiagnosticProperty::Double, MetricValue::Float, std::f64::MAX);
        test_from!(DiagnosticProperty::Double, MetricValue::Float, std::f64::MIN);
        test_from!(DiagnosticProperty::Bool, MetricValue::Bool, true);
        test_from!(DiagnosticProperty::Bool, MetricValue::Bool, false);
        let diagnostic_array = ArrayContent::Values(vec![1.5, 2.5, 3.5]);
        test_from_to!(
            DiagnosticProperty::DoubleArray,
            MetricValue::Vector,
            diagnostic_array,
            vec![MetricValue::Float(1.5), MetricValue::Float(2.5), MetricValue::Float(3.5)]
        );
        let diagnostic_array = ArrayContent::Values(vec![1, 2, 3]);
        test_from_to!(
            DiagnosticProperty::IntArray,
            MetricValue::Vector,
            diagnostic_array,
            vec![MetricValue::Int(1), MetricValue::Int(2), MetricValue::Int(3)]
        );
        let diagnostic_array = ArrayContent::Values(vec![1, 2, 3]);
        test_from_to!(
            DiagnosticProperty::UintArray,
            MetricValue::Vector,
            diagnostic_array,
            vec![MetricValue::Int(1), MetricValue::Int(2), MetricValue::Int(3)]
        );
    }

    // TODO(fxbug.dev/58922): Modify or probably delete this function after better error design.
    #[test]
    fn test_missing_hacks() -> Result<(), Error> {
        macro_rules! eval {
            ($e:expr) => {
                MetricState::evaluate_math(&config::parse::parse_expression($e)?)
            };
        }
        assert_eq!(eval!("Missing(2>'a')"), MetricValue::Bool(true));
        assert_eq!(eval!("Missing([])"), MetricValue::Bool(true));
        assert_eq!(eval!("Missing([2>'a'])"), MetricValue::Bool(true));
        assert_eq!(eval!("Missing([2>'a', 2>'a'])"), MetricValue::Bool(false));
        assert_eq!(eval!("Missing([2>1])"), MetricValue::Bool(false));
        assert_eq!(eval!("Or(Missing(2>'a'), 2>'a')"), MetricValue::Bool(true));
        Ok(())
    }

    // Correct operation of annotations is tested via annotation_tests.triage.
}
