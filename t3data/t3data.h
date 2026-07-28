#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif

// 取消可能存在的 min/max 宏定义
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

#include <iostream>
#include <cstdio>
#include <ctime>
#include <string>
#include <sstream>
#include <iomanip>

#include "HttpClient.h" //引入HttpClient http请求库，不支持https
#include "MD5.h"
#include "Base64.h" // 使用Base64自定义编码集，不依赖任何库
#include <thread>
#include "HttpClient.cpp"
#include "jsoncpp.h"
#include "t3config.h"

//static std::string T3data_Host = "http://w.t3data.net"; //t3网络验证线路地址
//static std::string T3data_AppKey = "35fac34e9ca6f281515dda7c13f6f4cd";//t3网络验证程序应用密钥
//static std::string T3data_Base64key = "rF2k3X4OYNAJ/djutHWPa0I9gCshSolfi7G6xwezyUpDQ5Lcv8B+qKVnmZbRM1TE";//t3网络验证程序Base64自定义编码集
//static std::string T3data_Path_SingleLogin = "F331E88A89DB825F";//t3网络验证程序单码卡密登录API调用路径
//static std::string T3data_Path_IsSingleLoginStatus = "2BB839E0C52E1669";//t3网络验证程序单码卡密登录状态查询（心跳验证）API调用路径
//static std::string T3data_Path_GetProgramNotice = "363D924039E9E7CA";//t3网络验证程序获取公告API调用路径
//static std::string T3data_Path_GetProgramVersionNumber = "D9A4BE8D25967577";//t3网络验证程序获取版本号API调用路径
//static std::string T3data_Path_GetValueContent = "A650BE5DE9763208";//t3网络验证程序获取变量内容API调用路径



class T3DATA {
	std::string requestApi;//请求接口地址
	std::map<std::string, std::string> requestParams;//请求参数
	std::string request_safe_code = getRandomString("", 32);//请求随机安全码-防劫持验证用
	std::string responseData;//响应数据(发送请求后这里则会被赋值)
	std::string s;//数据签名
#if defined(T3data_UsingHttps)
#else
	HTTPClient http;//本次请求http对象(发送请求后这里则会被赋值) - HttpClient
	HTTPRequest request;//本次请求request对象(发送请求后这里则会被赋值) - HttpClient
	HTTPResponse response;//本次请求response响应对象(发送请求后这里则会被赋值) - HttpClient
#endif

public:
	//设置请求接口地址
	virtual void setRequestApi(std::string api);

	//添加请求参数
	virtual void addRequestParam(std::string key, std::string value);

	//发送请求，若请求成功返回true
	virtual bool sendRequest();

	//获取响应数据
	std::string getResponseData();

	//获取响应数据json对象
	virtual Json::Value getResponseJsonObject();

	//解密数据
	virtual std::string decrypt(std::string &str);

	//加密数据
	virtual std::string encrypt(std::string &str);

	//请求安全码验证-响应数据防劫持验证
	virtual bool requestSafeCodeVerify();

	//响应数据签名验证
	virtual bool requestDataSignatureVerify();

	//响应数据时差验证
	virtual bool requestDataTimeDifferenceVerify();

	//统一参数中转处理，返回最终处理完的必要参数、签名、加密的参数map
	virtual std::map<std::string, std::string> paramsHandle(std::map<std::string, std::string> params);

	//统一参数中转处理，返回最终处理完的必要参数、签名、加密的参数字符串
	virtual std::string paramsHandleString(std::map<std::string, std::string> params);

	//获取计算参数签名字符串
	virtual std::string getParamsSignString(std::map<std::string, std::string> params);

	//获取当前时间戳
	virtual int getTimestamp();

	//获取日期格式时间，不带秒
	virtual std::string getCurrentDateTimeString();

	//获取字符串md5值
	virtual std::string getStringMd5(std::string strPlain);

	//获取随机字符串
	virtual std::string getRandomString(std::string strCharElem, int nOutStrLen);

	//解析json字符串
	virtual Json::Value parseJSON(const std::string& jsonString);

	//字符串转十六进制
	virtual std::string hexStringToAscii(const std::string& hexString);

	//十六进制转字符串
	virtual std::string asciiToHexString(const std::string& asciiString);
};

//设置请求接口地址
void T3DATA::setRequestApi(std::string api) {
	requestApi = api;
}

//添加请求参数
void T3DATA::addRequestParam(std::string key, std::string value) {
	requestParams[key] = value;
}

//发送请求，若请求成功返回true
bool T3DATA::sendRequest() {

	std::string paramsHandleStringResult = paramsHandleString(requestParams);;

	bool responseResult = false;


	std::string strData;
	request.url = T3Config::Host + "/" + requestApi + "?" + paramsHandleStringResult;
	if (http.get(request, response, strData))
	{
		responseData = strData;
		responseResult = true;
	}





	return responseResult;
}

std::string T3DATA::decrypt(std::string &str) {
	return Base64::decrypt(str,T3Config::Base64key,0); // 使用Base64自定义编码集，不依赖任何库，这里使用自定义编码集解密
}

std::string T3DATA::encrypt(std::string &str) {
	return asciiToHexString(Base64::encrypt(str,T3Config::Base64key)); // 使用Base64自定义编码集，不依赖任何库，这里使用自定义编码集加密并转为十六进制
}

//获取响应数据
std::string T3DATA::getResponseData() {
	std::string cleartext = decrypt(responseData);
	return cleartext;
}

//解析json字符串
Json::Value T3DATA::getResponseJsonObject() {
	Json::Value root;
	Json::Reader reader;

	if (reader.parse(getResponseData(), root)) {
		return root;
	} else {
		return Json::Value();
	}
}

//请求安全码验证-响应数据防劫持验证
bool T3DATA::requestSafeCodeVerify() {
	Json::Value responseJson = getResponseJsonObject();//获取响应json对象
	if (request_safe_code == responseJson["safe_code"].asString()) {
		return true;
	}
	return false;
}

//获取当前日期格式时间 不带秒
std::string T3DATA::getCurrentDateTimeString() {
    time_t rawTime;
    struct tm *timeInfo;

    time(&rawTime);
    timeInfo = localtime(&rawTime);

    int year = timeInfo->tm_year + 1900;
    int month = timeInfo->tm_mon + 1;
    int day = timeInfo->tm_mday;
    int hour = timeInfo->tm_hour;
    int minute = timeInfo->tm_min;

    std::stringstream ss;
    ss << std::setfill('0') << std::setw(4) << year << std::setw(2) << month << std::setw(2) << day
       << std::setw(2) << hour << std::setw(2) << minute;

    return ss.str();
}
//响应数据签名验证
bool T3DATA::requestDataSignatureVerify() {
	Json::Value responseJson = getResponseJsonObject();//获取响应json对象
	std::string token = responseJson["token"].asString();


	if (getStringMd5(responseJson["id"].asString() + T3Config::AppKey + s + responseJson["end_time"].asString() + getCurrentDateTimeString()) == token) {//响应数据验签成功
		return true;
	}
	return false;
}

//响应数据时差验证
bool T3DATA::requestDataTimeDifferenceVerify() {
	Json::Value responseJson = getResponseJsonObject();//获取响应json对象
	int server_time = responseJson["time"].asInt();
	int allow_time_scope_minimum = server_time - 30;
	int allow_time_scope_maximum = server_time + 30;
	int timestamp = getTimestamp();
	if (timestamp > allow_time_scope_maximum || timestamp < allow_time_scope_minimum) {
		return false;
	}
	return true;
}

//统一参数中转处理，返回最终处理完的必要参数、签名、加密的参数map
std::map<std::string, std::string> T3DATA::paramsHandle(std::map<std::string, std::string> params) {
	params["safe_code"] = request_safe_code;//增加请求防劫持验证参数
	params["t"] = std::to_string(getTimestamp()); //增加签名必要参数-时间戳
	
	
	std::map<std::string, std::string> encryptionRequestParams;
	//开始参数加密处理
	for (std::map<std::string, std::string>::iterator it = params.begin(); it != params.end(); ++it) {
		if (it->first != "") {
			encryptionRequestParams[it->first.c_str()] = encrypt(it->second);
		}
	}
	
	return encryptionRequestParams;
}

//统一参数中转处理，返回最终处理完的必要参数、签名、加密的参数字符串
std::string T3DATA::paramsHandleString(std::map<std::string, std::string> params) {
	std::map<std::string, std::string> encryptionRequestParams = paramsHandle(params);
	std::string paramsStr = "";
	for (std::map<std::string, std::string>::iterator it = encryptionRequestParams.begin(); it != encryptionRequestParams.end(); ++it) {
		if (paramsStr != "") {
			paramsStr.append("&");
		}
		paramsStr.append(it->first + "=" + it->second);
	}
	s = paramsStr + "&" + T3Config::AppKey;
	std::string ss = getStringMd5(s);
	return paramsStr + "&s=" + encrypt(ss);
}

//获取计算参数签名字符串
std::string T3DATA::getParamsSignString(std::map<std::string, std::string> params) {
	std::string signStr = "";
	for (std::map<std::string, std::string>::iterator it = params.begin(); it != params.end(); ++it) {
		if (it->first != "" && it->second != "") {
			if (signStr != "") {
				signStr.append("&");
			}
			signStr.append(it->first + "=" + encrypt(it->second));
		}
	}
	return signStr;
}

//获取当前时间戳
int T3DATA::getTimestamp() {
	time_t timet;
	timet = time(NULL);
	int timestamp = time(&timet);
	return timestamp;
}

//获取字符串md5值
std::string T3DATA::getStringMd5(std::string strPlain)
{
	unsigned char decrypt[16];
	MD5_CTX mdContext;
	int bytes;
	unsigned char data[1024];

	MD5Init(&mdContext);
	MD5Update(&mdContext, (unsigned char*)const_cast<char*>(strPlain.c_str()), strPlain.size());
	MD5Final(&mdContext, decrypt);

	std::string md5;
	char buf[3];
	for (int i = 0; i < 16; i++)
	{
		sprintf(buf, "%02x", decrypt[i]);
		md5.append(buf);
	}
	return md5;
}

//获取随机字符串
std::string T3DATA::getRandomString(std::string strCharElem, int nOutStrLen)
{
	if (strCharElem == "") {
		strCharElem = "abcdefghigklmnopqrstuvwxyz";
	}
	std::string strRet;
	strRet.insert(0, nOutStrLen, '\0');
	srand((unsigned)time(0));
	int iRand = 0;
	for (int i = 0; i < nOutStrLen; ++i)
	{
		iRand = rand() % strCharElem.length();
		strRet[i] = strCharElem[iRand];
	}
	return strRet;
}

//使用jsoncpp库解析json
Json::Value T3DATA::parseJSON(const std::string& jsonString) {
	Json::Value root;
	Json::Reader reader;

	if (reader.parse(jsonString, root)) {
		return root;
	} else {
		return Json::Value();
	}
}

//16进制字符串转10进制字符串
std::string T3DATA::hexStringToAscii(const std::string& hexString) {
    std::string asciiString;
    
    for (size_t i = 0; i < hexString.length(); i += 2) {
        std::string hexByte = hexString.substr(i, 2);
        int asciiValue;
        sscanf(hexByte.c_str(), "%2X", &asciiValue);
        asciiString.push_back(static_cast<char>(asciiValue));
    }
    
    return asciiString;
}

//10进制字符串转16进制字符串
std::string T3DATA::asciiToHexString(const std::string& asciiString) {
	std::string hexString;
	
	for (size_t i = 0; i < asciiString.length(); ++i) {
		char asciiByte = asciiString[i];
		char hexByte[3];
		sprintf(hexByte, "%02X", asciiByte);
		hexString.append(hexByte);
	}
	
	return hexString;
}
