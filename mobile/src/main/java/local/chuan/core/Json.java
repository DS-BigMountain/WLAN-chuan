package local.chuan.core;

import java.util.*;

/** Minimal, bounded JSON codec shared by Android and the protocol integration harness. */
public final class Json {
    private Json() {}
    public static Map<String,Object> map(Object... pairs) {
        Map<String,Object> result=new LinkedHashMap<>();
        for(int i=0;i<pairs.length;i+=2)result.put(String.valueOf(pairs[i]),pairs[i+1]);
        return result;
    }
    public static String stringify(Object value) {
        if(value==null)return "null";
        if(value instanceof Boolean||value instanceof Number)return value.toString();
        if(value instanceof Map){StringJoiner out=new StringJoiner(",","{","}");for(Map.Entry<?,?> item:((Map<?,?>)value).entrySet())out.add(stringify(String.valueOf(item.getKey()))+":"+stringify(item.getValue()));return out.toString();}
        if(value instanceof Collection){StringJoiner out=new StringJoiner(",","[","]");for(Object item:(Collection<?>)value)out.add(stringify(item));return out.toString();}
        StringBuilder out=new StringBuilder("\"");
        for(char c:value.toString().toCharArray())switch(c){case '"':out.append("\\\"");break;case '\\':out.append("\\\\");break;case '\n':out.append("\\n");break;case '\r':out.append("\\r");break;case '\t':out.append("\\t");break;default:if(c<32)out.append(String.format(Locale.ROOT,"\\u%04x",(int)c));else out.append(c);}
        return out.append('"').toString();
    }
    public static Object parse(String source) {
        if(source.length()>1048576)throw new IllegalArgumentException("JSON 数据过长");
        Parser parser=new Parser(source);Object result=parser.value(0);parser.space();if(parser.at!=source.length())throw new IllegalArgumentException("JSON 尾部无效");return result;
    }
    @SuppressWarnings("unchecked") public static Map<String,Object> object(String source){Object value=parse(source);if(!(value instanceof Map))throw new IllegalArgumentException("需要 JSON 对象");return (Map<String,Object>)value;}
    public static String string(Map<String,Object> value,String key,String fallback){Object item=value.get(key);return item==null?fallback:String.valueOf(item);}
    public static long number(Map<String,Object> value,String key,long fallback){try{return Long.parseLong(string(value,key,String.valueOf(fallback)));}catch(Exception error){return fallback;}}
    static final class Parser {
        final String source;int at;
        Parser(String value){source=value;}
        void space(){while(at<source.length()&&Character.isWhitespace(source.charAt(at)))at++;}
        char take(){if(at>=source.length())throw new IllegalArgumentException("JSON 不完整");return source.charAt(at++);}
        Object value(int depth){
            if(depth>64)throw new IllegalArgumentException("JSON 嵌套过深");space();char c=take();
            if(c=='"')return quoted();
            if(c=='{'){Map<String,Object> out=new LinkedHashMap<>();space();if(at<source.length()&&source.charAt(at)=='}'){at++;return out;}for(;;){space();if(take()!='"')throw new IllegalArgumentException("JSON 键无效");String key=quoted();space();if(take()!=':')throw new IllegalArgumentException("JSON 分隔符无效");if(out.containsKey(key))throw new IllegalArgumentException("JSON 键重复");out.put(key,value(depth+1));space();c=take();if(c=='}')return out;if(c!=',')throw new IllegalArgumentException("JSON 对象无效");}}
            if(c=='['){List<Object> out=new ArrayList<>();space();if(at<source.length()&&source.charAt(at)==']'){at++;return out;}for(;;){out.add(value(depth+1));space();c=take();if(c==']')return out;if(c!=',')throw new IllegalArgumentException("JSON 数组无效");}}
            at--;if(source.startsWith("true",at)){at+=4;return true;}if(source.startsWith("false",at)){at+=5;return false;}if(source.startsWith("null",at)){at+=4;return null;}
            int begin=at;while(at<source.length()&&"0123456789-.eE+".indexOf(source.charAt(at))>=0)at++;String token=source.substring(begin,at);if(token.isEmpty())throw new IllegalArgumentException("JSON 数字无效");if(token.indexOf('.')>=0||token.indexOf('e')>=0||token.indexOf('E')>=0)return Double.parseDouble(token);return Long.parseLong(token);
        }
        String quoted(){StringBuilder out=new StringBuilder();for(;;){char c=take();if(c=='"')return out.toString();if(c=='\\'){c=take();switch(c){case '"':case '\\':case '/':out.append(c);break;case 'n':out.append('\n');break;case 'r':out.append('\r');break;case 't':out.append('\t');break;case 'b':out.append('\b');break;case 'f':out.append('\f');break;case 'u':if(at+4>source.length())throw new IllegalArgumentException("JSON 转义不完整");out.append((char)Integer.parseInt(source.substring(at,at+4),16));at+=4;break;default:throw new IllegalArgumentException("JSON 转义无效");}}else{if(c<32)throw new IllegalArgumentException("JSON 字符无效");out.append(c);}}}
    }
}
