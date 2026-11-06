use serde_json::Value;

pub fn to_string(v: &Value) -> String {
    let mut out = String::new();
    write(v, &mut out);
    out
}

pub fn number(f: f64) -> String {
    if f == 0.0 {
        return "0".into();
    }
    if !f.is_finite() {
        return "null".into();
    }
    let formatted = format!("{:e}", f.abs());
    let (mantissa, exp) = formatted.split_once('e').unwrap_or((&formatted, "0"));
    let digits: String = mantissa.chars().filter(|c| *c != '.').collect();
    let k = digits.len() as i32;
    let n = exp.parse::<i32>().unwrap_or(0) + 1;
    let body = if k <= n && n <= 21 {
        format!("{digits}{}", "0".repeat((n - k) as usize))
    } else if 0 < n && n <= 21 {
        format!("{}.{}", &digits[..n as usize], &digits[n as usize..])
    } else if -6 < n && n <= 0 {
        format!("0.{}{digits}", "0".repeat((-n) as usize))
    } else {
        let e = n - 1;
        let sign = if e < 0 { '-' } else { '+' };
        let head = if k == 1 {
            digits.clone()
        } else {
            format!("{}.{}", &digits[..1], &digits[1..])
        };
        format!("{head}e{sign}{}", e.abs())
    };
    if f < 0.0 {
        format!("-{body}")
    } else {
        body
    }
}

pub fn js_string(v: &Value) -> String {
    match v {
        Value::Null => "null".into(),
        Value::Bool(b) => b.to_string(),
        Value::Number(n) => number(n.as_f64().unwrap_or(0.0)),
        Value::String(s) => s.clone(),
        Value::Array(a) => a
            .iter()
            .map(|x| {
                if x.is_null() {
                    String::new()
                } else {
                    js_string(x)
                }
            })
            .collect::<Vec<_>>()
            .join(","),
        Value::Object(_) => "[object Object]".into(),
    }
}

fn write(v: &Value, out: &mut String) {
    match v {
        Value::Number(n) => out.push_str(&number(n.as_f64().unwrap_or(0.0))),
        Value::Array(a) => {
            out.push('[');
            for (i, x) in a.iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                write(x, out);
            }
            out.push(']');
        }
        Value::Object(o) => {
            out.push('{');
            for (i, (k, x)) in o.iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                out.push_str(&serde_json::to_string(k).unwrap_or_default());
                out.push(':');
                write(x, out);
            }
            out.push('}');
        }
        other => out.push_str(&serde_json::to_string(other).unwrap_or_default()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn numbers_match_javascript() {
        let cases = [
            (1.0, "1"),
            (-0.0, "0"),
            (0.1, "0.1"),
            (1.5e-7, "1.5e-7"),
            (0.000001, "0.000001"),
            (1e21, "1e+21"),
            (123456789012345680000.0, "123456789012345680000"),
            (-2.5, "-2.5"),
            (12345678901234567890.0, "12345678901234567000"),
            (72.25, "72.25"),
        ];
        for (f, s) in cases {
            assert_eq!(number(f), s, "{f}");
        }
    }

    #[test]
    fn objects_keep_order_and_reformat_numbers() {
        let v: Value =
            serde_json::from_str(r#"{"b":1.0,"a":[2.50,null,"x\u0001"],"c":{"d":true}}"#).unwrap();
        assert_eq!(
            to_string(&v),
            r#"{"b":1,"a":[2.5,null,"x\u0001"],"c":{"d":true}}"#
        );
        assert_eq!(js_string(&serde_json::json!([1, null, "a"])), "1,,a");
    }
}
